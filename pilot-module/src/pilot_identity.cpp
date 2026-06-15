#include "pilot_impl.h"
#include "pilot_skill.h"
#include "pilot_crypto.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_mode.h"
#include <sqlite3.h>
#include <sstream>
#include <chrono>
#include <fstream>
#include <sys/stat.h>
#include <cstdlib>
#include <thread>
#include <functional>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <openssl/evp.h>
#include <QString>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

// ---- funding helpers (LP-0008 sovereign pinata funding) ----
namespace {
constexpr const char* kWalletModule = "logos_execution_zone";
// Well-known dev pinata faucet account (base58). Decodes to cafe…cafe (32 bytes).
constexpr const char* kPinataBase58 = "EfQhKQAkX2FJiwNii2WFQsGndjvF1Mzd7RuVe7QdPLw7";

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

// 16-byte little-endian hex of an unsigned value (amount or pinata solution).
std::string u128LeHex(uint64_t value) {
    char hex[33];
    for (int i = 0; i < 16; ++i) {
        uint8_t b = (i < 8) ? static_cast<uint8_t>((value >> (8 * i)) & 0xFF) : 0;
        snprintf(hex + i * 2, 3, "%02x", b);
    }
    hex[32] = '\0';
    return std::string(hex);
}

// Pinata proof-of-work: data = [difficulty(1)][seed(32)]. Find the smallest u128
// solution where SHA256(seed ++ solution_LE16) has `difficulty` leading zero bytes.
// Returns the solution as 16-byte little-endian hex, or "" on bad input.
std::string computePinataSolution(const std::string& dataHex) {
    std::vector<uint8_t> data = hexToBytes(dataHex);
    if (data.size() < 33) return {};
    const int difficulty = data[0];
    uint8_t buf[48];
    std::memcpy(buf, data.data() + 1, 32);   // seed
    const EVP_MD* md = EVP_sha256();
    for (uint64_t sol = 0; sol != 0ULL - 1; ++sol) {
        for (int i = 0; i < 16; ++i)
            buf[32 + i] = (i < 8) ? static_cast<uint8_t>((sol >> (8 * i)) & 0xFF) : 0;
        uint8_t h[EVP_MAX_MD_SIZE];
        unsigned int hlen = 0;
        if (EVP_Digest(buf, sizeof(buf), h, &hlen, md, nullptr) != 1) return {};
        bool ok = true;
        for (int i = 0; i < difficulty; ++i) { if (h[i] != 0) { ok = false; break; } }
        if (ok) return u128LeHex(sol);
    }
    return {};
}
}  // namespace

bool PilotImpl::initialize(const std::string& dataDir) {
    if (initialized_) return true;

    initDatabase(dataDir);
    inboundTasksRecover();     // fail inbound A2A tasks that died with the previous process
    initLLM();

    if (loadIdentity()) {
        initialized_ = true;
        initWallet();              // reopen the on-disk wallet for this process
        // Guard against pilot.db / wallet divergence: confirm the wallet actually holds
        // the saved account. If not (corrupt/replaced wallet), recover by recreating a
        // matching identity instead of pointing at an account the wallet no longer has.
        bool walletHasAccount = false;
        auto* w = logosAPI_ ? logosAPI_->getClient(kWalletModule) : nullptr;
        if (w && !agentAccountId_.empty()) {
            QVariant k = w->invokeRemoteMethod(kWalletModule, "get_private_account_keys",
                QString::fromStdString(agentAccountId_));
            walletHasAccount = (!k.isNull() && !k.toString().isEmpty());
        }
        if (!walletHasAccount) {
            qWarning() << "[pilot] initialize: wallet missing saved account -> recovering identity";
            resetStaleIdentity();
            createIdentity();      // fresh, consistent pilot.db + wallet pair
        }
        recoverPendingTransactions();
        fundAgentIfNeeded();       // idempotent, best-effort
        return true;
    }

    if (createIdentity()) {        // calls initWallet() internally
        initialized_ = true;
        fundAgentIfNeeded();       // idempotent, best-effort
        return true;
    }

    return false;
}

bool PilotImpl::isInitialized() {
    return initialized_;
}

bool PilotImpl::loadIdentity() {
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT npk, account_id FROM agent_identity WHERE id = 1;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        agentNpk_ = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        agentAccountId_ = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        QJsonDocument npkDoc = QJsonDocument::fromJson(QByteArray::fromStdString(agentNpk_));
        if (npkDoc.isObject() && npkDoc.object().contains("viewing_public_key"))
            agentViewingKey_ = npkDoc.object()["viewing_public_key"].toString().toStdString();
        else
            agentViewingKey_ = agentNpk_;

        found = true;
    }
    sqlite3_finalize(stmt);

    if (found) {
        rc = sqlite3_prepare_v2(db_,
            "SELECT conversation_id FROM owner_channel WHERE id = 1;",
            -1, &stmt, nullptr);
        if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            ownerChannelId_ = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        }
        if (stmt) sqlite3_finalize(stmt);

        rc = sqlite3_prepare_v2(db_,
            "SELECT key, value FROM config;",
            -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                std::string val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (key == "spend_limit_per_tx") spendLimitPerTx_ = std::stoll(val);
                else if (key == "spend_limit_per_period") spendLimitPerPeriod_ = std::stoll(val);
                else if (key == "spend_period_seconds") spendPeriodSeconds_ = std::stoll(val);
                else if (key == "owner.npk") ownerNpk_ = val;
                else if (key == "owner.name") ownerName_ = val;
                else if (key == "ecies.pub") agentEciesPub_ = val;
                else if (key == "ecies.priv") agentEciesPriv_ = val;
                else if (key == "llm.provider") llmProvider_ = val;
                else if (key == "llm.model") llmModel_ = val;
                else if (key == "llm.api_key") {
                    if (llmProvider_ == "anthropic")
                        setenv("ANTHROPIC_API_KEY", val.c_str(), 1);
                    else if (llmProvider_ == "deepseek")
                        setenv("DEEPSEEK_API_KEY", val.c_str(), 1);
                    else if (llmProvider_ == "google")
                        setenv("GOOGLE_API_KEY", val.c_str(), 1);
                    else if (llmProvider_ == "openrouter")
                        setenv("OPENROUTER_API_KEY", val.c_str(), 1);
                    else if (llmProvider_ == "groq")
                        setenv("GROQ_API_KEY", val.c_str(), 1);
                    else
                        setenv("OPENAI_API_KEY", val.c_str(), 1);
                }
            }
        }
        if (stmt) sqlite3_finalize(stmt);

        if (!llmProvider_.empty() || !llmModel_.empty())
            initLLM();
    }

    return found;
}

// Wipe the saved identity + funded flag when the on-disk wallet has diverged from
// pilot.db (e.g. the wallet file was unreadable and had to be recreated). Keeps the
// pilot's notebook and the wallet's keyring consistent: the agent then recreates a
// fresh matching identity and re-funds, instead of pointing at an account the wallet
// no longer has (which is what caused ACCOUNT_NOT_FOUND / KEY_NOT_FOUND).
void PilotImpl::resetStaleIdentity() {
    if (db_) {
        sqlite3_exec(db_, "DELETE FROM agent_identity WHERE id=1;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "DELETE FROM config WHERE key='funded';", nullptr, nullptr, nullptr);
    }
    agentAccountId_.clear();
    agentNpk_.clear();
    agentViewingKey_.clear();
}

// Persist the wallet and keep a backup copy of its storage file. This wallet has no
// seed-phrase / private-key export — the storage file IS the only key backup — so we
// keep a second copy that initWallet() can restore from if the main file is lost.
void PilotImpl::backupWallet() {
    if (!logosAPI_ || dataDir_.empty()) return;
    if (auto* wallet = logosAPI_->getClient(kWalletModule))
        wallet->invokeRemoteMethod(kWalletModule, "save");
    std::string storagePath = dataDir_ + "/wallet_storage.json";
    std::ifstream src(storagePath, std::ios::binary);
    if (src.good()) {
        std::ofstream dst(storagePath + ".bak", std::ios::binary | std::ios::trunc);
        dst << src.rdbuf();
    }
}

bool PilotImpl::initWallet() {
    if (walletOpened_) return true;   // idempotent: handle persists in the module process
    if (!logosAPI_) { qWarning() << "[pilot] initWallet: logosAPI_ is NULL"; return false; }

    auto* wallet = logosAPI_->getClient(kWalletModule);
    if (!wallet) { qWarning() << "[pilot] initWallet: getClient returned null"; return false; }

    for (int i = 0; i < 20 && !wallet->isConnected(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (!wallet->isConnected()) { qWarning() << "[pilot] initWallet: wallet not connected after 5s"; return false; }

    std::string configPath = dataDir_ + "/wallet_config.json";
    // Storage MUST be a file path (wallet_ffi_open/create_new write a file, not a dir).
    std::string storagePath = dataDir_ + "/wallet_storage.json";

    std::string sequencerAddr = "http://127.0.0.1:3040";   // v0.1.2 standalone sequencer
    if (const char* env = std::getenv("PILOT_SEQUENCER_ADDR"))
        sequencerAddr = env;

    // Always (re)write the config so the sequencer address is correct for this run.
    {
        std::ofstream cf(configPath, std::ios::trunc);
        if (cf.is_open()) {
            QJsonObject walletCfg;
            walletCfg["sequencer_addr"] = QString::fromStdString(sequencerAddr);
            walletCfg["seq_poll_timeout"] = QString("30s");
            walletCfg["seq_tx_poll_max_blocks"] = 15;
            walletCfg["seq_poll_max_retries"] = 10;
            walletCfg["seq_block_poll_max_amount"] = 100;
            walletCfg["initial_accounts"] = QJsonArray();
            cf << QJsonDocument(walletCfg).toJson(QJsonDocument::Indented).toStdString();
            cf.close();
        }
    }

    std::string backupPath = storagePath + ".bak";

    auto tryOpen = [&](const std::string& path) -> bool {
        QVariant r = wallet->invokeRemoteMethod(
            kWalletModule, "open",
            QString::fromStdString(configPath),
            QString::fromStdString(path), Timeout(15000));
        return (!r.isNull() && r.toInt() == 0);
    };
    auto copyFile = [](const std::string& from, const std::string& to) {
        std::ifstream src(from, std::ios::binary);
        if (!src.good()) return;
        std::ofstream dst(to, std::ios::binary | std::ios::trunc);
        dst << src.rdbuf();
    };
    bool storageExists = std::ifstream(storagePath).good();
    bool backupExists  = std::ifstream(backupPath).good();

    // 1. Normal restart: open the main wallet file. (Most common real-world path.)
    if (storageExists && tryOpen(storagePath)) { walletOpened_ = true; return true; }

    // 2. Main file missing/unreadable but a backup exists -> RESTORE it. Same account,
    //    same keys, same funds — never lose a real user's money to a fresh account.
    if (backupExists && tryOpen(backupPath)) {
        copyFile(backupPath, storagePath);   // promote the restored copy to the main file
        qWarning() << "[pilot] initWallet: restored wallet from backup";
        walletOpened_ = true; return true;
    }

    // 3. A storage file exists but neither it nor a backup could be opened -> it is
    //    corrupt/incompatible. NEVER overwrite it silently (that was the bug). Move it
    //    aside for forensics, and reset the now-orphaned identity so pilot.db and the
    //    fresh wallet stay consistent.
    if (storageExists) {
        std::rename(storagePath.c_str(), (storagePath + ".corrupt").c_str());
        qWarning() << "[pilot] initWallet: wallet unreadable; moved aside + resetting identity";
        resetStaleIdentity();
    }

    // 4. Genuine first run (or unrecoverable) -> create a fresh wallet.
    std::string walletName = "pilot_" + std::to_string(
        std::hash<std::string>{}(dataDir_) & 0xFFFFFFFF);
    QVariant createResult = wallet->invokeRemoteMethod(
        kWalletModule, "create_new",
        QString::fromStdString(configPath),
        QString::fromStdString(storagePath),
        QString::fromStdString(walletName), Timeout(15000));
    qWarning() << "[pilot] initWallet: create_new result:" << createResult;
    if (!createResult.isNull() && createResult.toInt() == 0) { walletOpened_ = true; return true; }
    return false;
}

bool PilotImpl::createIdentity() {
    if (!logosAPI_) return false;

    if (!initWallet()) return false;

    auto* wallet = logosAPI_->getClient(kWalletModule);
    if (!wallet) return false;

    QVariant result = wallet->invokeRemoteMethod(
        kWalletModule, "create_account_private");
    if (result.isNull() || result.toString().isEmpty()) return false;

    agentAccountId_ = result.toString().toStdString();

    QVariant keysResult = wallet->invokeRemoteMethod(
        kWalletModule, "get_private_account_keys",
        QString::fromStdString(agentAccountId_));
    if (keysResult.isNull()) return false;

    agentNpk_ = keysResult.toString().toStdString();

    QJsonDocument npkDoc = QJsonDocument::fromJson(keysResult.toString().toUtf8());
    if (npkDoc.isObject() && npkDoc.object().contains("viewing_public_key"))
        agentViewingKey_ = npkDoc.object()["viewing_public_key"].toString().toStdString();
    else
        agentViewingKey_ = agentNpk_;

    ECIESKeypair kp = generateECIESKeypair();
    agentEciesPub_ = kp.publicKeyHex;
    agentEciesPriv_ = kp.privateKeyHex;

    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    std::string timestamp = std::to_string(seconds);

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO agent_identity (id, npk, account_id, created_at) VALUES (1, ?, ?, ?);",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, agentNpk_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, agentAccountId_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, timestamp.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;

    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO config (key, value) VALUES ('ecies.pub', ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, agentEciesPub_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO config (key, value) VALUES ('ecies.priv', ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, agentEciesPriv_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return true;
}

// Fund the agent's private account from the dev pinata faucet, once.
// Flow (proven against LEZ v0.1.2): create+register a public account, claim the
// pinata into it (solving the PoW), then shielded-transfer into the agent's
// private account. Idempotent via the 'funded' config flag; best-effort (never
// fatal to startup). NOTE: if the chain is wiped, also clear pilot.db so this re-runs.
bool PilotImpl::fundAgentIfNeeded() {
    if (!logosAPI_ || agentAccountId_.empty() || !db_) return false;

    // Idempotency check.
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT value FROM config WHERE key='funded';",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            bool funded = (sqlite3_step(stmt) == SQLITE_ROW &&
                           std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))) == "1");
            sqlite3_finalize(stmt);
            if (funded) return true;
        }
    }

    if (!initWallet()) { qWarning() << "[pilot] fund: wallet not open"; return false; }
    auto* wallet = logosAPI_->getClient(kWalletModule);
    if (!wallet) return false;

    const int64_t fundAmount = 100;   // tokens to move into the agent's private account

    auto syncToHead = [&]() {
        QVariant h = wallet->invokeRemoteMethod(kWalletModule, "get_current_block_height");
        if (!h.isNull())
            wallet->invokeRemoteMethod(kWalletModule, "sync_to_block",
                                       QString::number(h.toLongLong()), Timeout(30000));
    };
    auto ok = [](const QVariant& v) {
        if (v.isNull()) return false;
        QJsonDocument d = QJsonDocument::fromJson(v.toString().toUtf8());
        return d.isObject() && d.object().value("success").toBool();
    };

    // 1. Fresh public account + initialise it on-chain.
    QVariant pubV = wallet->invokeRemoteMethod(kWalletModule, "create_account_public");
    std::string pubId = pubV.isNull() ? std::string() : pubV.toString().toStdString();
    if (pubId.empty()) { qWarning() << "[pilot] fund: create_account_public failed"; return false; }
    if (!ok(wallet->invokeRemoteMethod(kWalletModule, "register_public_account",
                                       QString::fromStdString(pubId), Timeout(30000)))) {
        qWarning() << "[pilot] fund: register_public_account failed"; return false;
    }
    // Wait for the register tx to be mined — claim_pinata requires an initialised
    // recipient on-chain, else the claim is accepted but never credits.
    {
        QVariant s = wallet->invokeRemoteMethod(kWalletModule, "get_current_block_height");
        long long start = s.isNull() ? 0 : s.toLongLong();
        for (int i = 0; i < 60; ++i) {
            syncToHead();
            QVariant h = wallet->invokeRemoteMethod(kWalletModule, "get_current_block_height");
            if (!h.isNull() && h.toLongLong() >= start + 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }

    // 2. Resolve the pinata id, fetch its data, solve the PoW.
    QVariant pinV = wallet->invokeRemoteMethod(kWalletModule, "account_id_from_base58",
                                               QString(kPinataBase58), Timeout(15000));
    std::string pinataHex = pinV.isNull() ? std::string() : pinV.toString().toStdString();
    if (pinataHex.empty()) { qWarning() << "[pilot] fund: pinata id resolve failed"; return false; }

    QVariant accV = wallet->invokeRemoteMethod(kWalletModule, "get_account_public",
                                               QString::fromStdString(pinataHex), Timeout(15000));
    QJsonDocument accDoc = accV.isNull() ? QJsonDocument() : QJsonDocument::fromJson(accV.toString().toUtf8());
    std::string dataHex = accDoc.isObject() ? accDoc.object().value("data").toString().toStdString() : std::string();
    std::string solHex = computePinataSolution(dataHex);
    if (solHex.empty()) { qWarning() << "[pilot] fund: bad pinata data / no solution"; return false; }

    // 3. Claim the pinata into the public account.
    if (!ok(wallet->invokeRemoteMethod(kWalletModule, "claim_pinata",
                                       QString::fromStdString(pinataHex),
                                       QString::fromStdString(pubId),
                                       QString::fromStdString(solHex), Timeout(60000)))) {
        qWarning() << "[pilot] fund: claim_pinata failed"; return false;
    }

    // Wait for the claim tx to be mined and credited before spending it.
    bool credited = false;
    for (int i = 0; i < 60 && !credited; ++i) {
        syncToHead();
        QVariant balV = wallet->invokeRemoteMethod(kWalletModule, "get_balance",
                                                   QString::fromStdString(pubId), QVariant(true));
        if (!balV.isNull() && balV.toString().toLongLong() >= fundAmount) credited = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (!credited) { qWarning() << "[pilot] fund: pinata claim not credited in time"; return false; }

    // 4. Shielded transfer public -> agent's private account (generates a ZK proof).
    if (!ok(wallet->invokeRemoteMethod(kWalletModule, "transfer_shielded_owned",
                                       QString::fromStdString(pubId),
                                       QString::fromStdString(agentAccountId_),
                                       QString::fromStdString(u128LeHex(fundAmount)), Timeout(120000)))) {
        qWarning() << "[pilot] fund: transfer_shielded_owned failed"; return false;
    }
    syncToHead();

    // 5. Mark funded.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO config (key, value) VALUES ('funded', '1');",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    qWarning() << "[pilot] fund: agent private account funded with" << fundAmount;
    backupWallet();   // persist + keep a recovery copy of the now-funded wallet
    return true;
}

std::string PilotImpl::getAgentNpk() {
    return agentNpk_;
}

std::string PilotImpl::getAccountId() {
    return agentAccountId_;
}

std::string PilotImpl::walletBalance() {
    if (!logosAPI_ || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";

    auto* wallet = logosAPI_->getClient(kWalletModule);
    if (!wallet) return "{\"error\": \"wallet module unavailable\"}";

    // Sync to chain head so the balance reflects the latest blocks.
    QVariant head = wallet->invokeRemoteMethod(kWalletModule, "get_current_block_height");
    if (!head.isNull())
        wallet->invokeRemoteMethod(kWalletModule, "sync_to_block",
                                   QString::number(head.toLongLong()), Timeout(30000));

    QVariant result = wallet->invokeRemoteMethod(
        kWalletModule, "get_balance",
        QString::fromStdString(agentAccountId_), QVariant(false));

    if (result.isNull())
        return "{\"error\": \"balance query failed\"}";

    QJsonObject obj;
    obj["balance"] = result.toString();
    obj["account"] = QString::fromStdString(agentAccountId_);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::walletHistory() {
    if (!logosAPI_ || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";
    if (!db_) return "{\"error\": \"database not open\"}";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT id, recipient, amount, state, created_at FROM spend_requests "
        "ORDER BY created_at DESC LIMIT 50;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return "{\"error\": \"query failed\"}";

    QJsonArray arr;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QJsonObject tx;
        tx["id"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        tx["recipient"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        tx["amount"] = static_cast<double>(sqlite3_column_int64(stmt, 2));
        tx["state"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        tx["created_at"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        arr.append(tx);
    }
    sqlite3_finalize(stmt);

    QJsonObject root;
    root["transactions"] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}
