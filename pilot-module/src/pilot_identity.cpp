#include "pilot_impl.h"
#include "pilot_skill.h"
#include "pilot_crypto.h"
// Generated per-build; typed client for lez_core (see pilot_impl.h).
#include "logos_sdk.h"
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
constexpr const char* kWalletModule = "lez_core";
// Catching the wallet up to chain head is SLOW and gets slower as the chain grows: a measured
// sync of ~3500 blocks took 37.2s, against a 30s cap that was silently cutting it short. Every
// caller shares this one generous ceiling so no sync site can quietly drift back under the real
// cost. It is only a ceiling — a fast sync returns immediately.
constexpr int kWalletSyncTimeoutMs = 600000;   // 10 minutes
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
    // Fetch the digest implementation ONCE and reuse one context. The previous form —
    // EVP_Digest(..., EVP_sha256(), ...) per candidate — makes OpenSSL 3 perform an implicit
    // algorithm fetch on every call, tens of microseconds each; at difficulty 3 (~16M
    // candidates) that turned a ~30 s search into 10+ minutes of a blocked module thread
    // (measured 2026-08-25: the pilot host at ~40 % CPU with no RPC traffic for 12 min while
    // the daemon timed out every call), which is what "funding needs several boots" was.
    EVP_MD* md = EVP_MD_fetch(nullptr, "SHA256", nullptr);
    if (!md) return {};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) { EVP_MD_free(md); return {}; }
    std::string result;
    for (uint64_t sol = 0; sol != 0ULL - 1; ++sol) {
        for (int i = 0; i < 16; ++i)
            buf[32 + i] = (i < 8) ? static_cast<uint8_t>((sol >> (8 * i)) & 0xFF) : 0;
        uint8_t h[EVP_MAX_MD_SIZE];
        unsigned int hlen = 0;
        if (EVP_DigestInit_ex(ctx, md, nullptr) != 1 ||
            EVP_DigestUpdate(ctx, buf, sizeof(buf)) != 1 ||
            EVP_DigestFinal_ex(ctx, h, &hlen) != 1) break;
        bool ok = true;
        for (int i = 0; i < difficulty; ++i) { if (h[i] != 0) { ok = false; break; } }
        if (ok) { result = u128LeHex(sol); break; }
    }
    EVP_MD_CTX_free(ctx);
    EVP_MD_free(md);
    return result;
}
}  // namespace

bool PilotImpl::initialize(const std::string& dataDir) {
    if (initialized_) return true;

    // Persistent, shared identity: PILOT_DATA_DIR (when set) overrides the caller's
    // data dir, so the CLI and Basecamp can both point at one durable location
    // (e.g. ~/.pilot) instead of their own caller-specific path (often /tmp, which is
    // wiped on reboot). Same env var -> same pilot.db + wallet_storage -> same agent.
    std::string effectiveDir = dataDir;
    if (const char* env = std::getenv("PILOT_DATA_DIR"); env && *env)
        effectiveDir = env;

    initDatabase(effectiveDir);
    inboundTasksRecover();     // fail inbound A2A tasks that died with the previous process
    outboundTasksRecover();    // re-arm/reconcile outbound A2A settlement after a restart (M7)
    // L7 — resolve/surface spends crash-stranded in EXECUTING. Placed here (NOT inside
    // recoverPendingTransactions, which is gated on the module context AND only runs on the loadIdentity()
    // branch) so it fires on every startup path, incl. degraded restart (no transport yet) and
    // createIdentity(). db_-only; runs strictly after outboundTasksRecover()'s step (2), preserving
    // the L7⊕L8 ordering invariant. sendToOwner() inside is a safe no-op without an owner channel.
    reconcileExecutingSpends();
    initLLM();

    // Runtime third-party skills (Usability #1). Builtins are already registered in the
    // ctor (registerBuiltinSkills), so this loads strictly ON TOP of them — a plugin can
    // ADD skills but, by the loader's name-clash guard, never SHADOW a builtin. OFF BY
    // DEFAULT: loadPlugins is inert unless the operator sets PILOT_ENABLE_PLUGINS, in
    // which case it scans an operator-TRUSTED directory whose contents run with full agent
    // privileges (PILOT_PLUGINS_DIR, else ~/.pilot/plugins). Best-effort: a bad plugin is
    // logged + skipped, never fatal to startup.
    if (registry_) {
        std::string pluginsDir;
        if (const char* pd = std::getenv("PILOT_PLUGINS_DIR"); pd && *pd)
            pluginsDir = pd;
        else if (const char* home = std::getenv("HOME"); home && *home)
            pluginsDir = std::string(home) + "/.pilot/plugins";
        if (!pluginsDir.empty())
            registry_->loadPlugins(pluginsDir);
    }

    if (loadIdentity()) {
        initialized_ = true;
        initWallet();              // reopen the on-disk wallet for this process
        // Guard against pilot.db / wallet divergence: confirm the wallet actually holds
        // the saved account. If not (corrupt/replaced wallet), recover by recreating a
        // matching identity instead of pointing at an account the wallet no longer has.
        // Only RECOVER when we can POSITIVELY confirm the wallet lacks the saved account.
        // If the wallet module isn't reachable (no client yet) we CANNOT verify divergence,
        // so keep the loaded identity rather than wiping a good one on an unverifiable
        // assumption — a transient wallet unavailability must never destroy the agent identity.
        bool couldCheckWallet = false;
        bool walletHasAccount = false;
        if (isContextReady() && !agentAccountId_.empty()) {
            logos::CallError kerr;
            std::string k = modules().lez_core.get_private_account_keys(agentAccountId_, &kerr);
            // A transport/module error means we could NOT check — it must never read as
            // "wallet lacks the account", or a transient outage would wipe a good identity.
            couldCheckWallet = kerr.code.empty();
            walletHasAccount = couldCheckWallet && !k.empty();
        }
        if (couldCheckWallet && !walletHasAccount) {
            qWarning() << "[pilot] initialize: wallet missing saved account -> recovering identity";
            resetStaleIdentity();
            createIdentity();      // fresh, consistent pilot.db + wallet pair
        }
        // Boot order: the identity now EXISTS, so this is the first moment the agent's own
        // topics can be named. Bring delivery up and subscribe here rather than relying on
        // initDeliveryModule() having been reached from somewhere else earlier — it fires
        // before identity load, and its run-once guard makes an early miss permanent
        // (2026-07-26: agents advertised inboxes they never listened on).
        //
        // What this subscribes depends on the restored a2a.open_for_hire flag: the shared
        // discovery channel always, the agent's own inbox(es) only if the owner had already
        // opened it for hire. So "I opened my agent" survives the reboot, and an agent that
        // was never opened comes back silent.
        initDeliveryModule();
        subscribeIdentityTopics();
        recoverPendingTransactions();
        fundAgentIfNeeded();       // idempotent, best-effort
        return true;
    }

    if (createIdentity()) {        // calls initWallet() internally
        initialized_ = true;
        // A brand-new identity is closed for hire by default — nobody has said otherwise yet.
        // This still subscribes the discovery channel, so a fresh agent can find peers to hire
        // before it ever offers itself for hire.
        initDeliveryModule();
        subscribeIdentityTopics();
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

        std::string eciesPrivStored;   // M2: captured raw; resolved after the cursor closes
        std::string encPrivStored;     // L1: captured raw; resolved after the cursor closes
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
                else if (key == "ecies.priv") eciesPrivStored = val;   // raw; resolved below
                else if (key == "enc.pub") agentEncPub_ = val;
                else if (key == "enc.priv") encPrivStored = val;       // L1: raw; resolved below
                // The owner's standing decision to take work from strangers. Absent == closed:
                // an agent that has never been told to open for hire stays off the market.
                else if (key == "a2a.open_for_hire") openForHire_ = (val == "1");
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

        // M2: resolve the at-rest ECIES private key AFTER the cursor closes (never
        // decrypt/rewrite mid-cursor). A wrapped key decrypts with PILOT_KEY_PASSPHRASE;
        // a legacy plaintext key is taken verbatim and re-wrapped in place when a
        // passphrase is now set (transparent migration — the identity is never
        // regenerated). A wrapped key with a missing/wrong passphrase leaves
        // agentEciesPriv_ empty + warns (A2A/owner ECDH degrades this run; the separate
        // wallet keys are unaffected, so the agent is never bricked).
        const char* keyPass = std::getenv("PILOT_KEY_PASSPHRASE");
        if (!eciesPrivStored.empty()) {
            if (isWrappedSecret(eciesPrivStored)) {
                if (keyPass && *keyPass) {
                    try { agentEciesPriv_ = unwrapSecret(eciesPrivStored, keyPass); }
                    catch (...) {
                        qWarning() << "[pilot] loadIdentity: ecies.priv decrypt FAILED "
                                      "(wrong PILOT_KEY_PASSPHRASE?); A2A/owner crypto unavailable this run";
                    }
                } else {
                    qWarning() << "[pilot] loadIdentity: ecies.priv is encrypted but "
                                  "PILOT_KEY_PASSPHRASE is unset; A2A/owner crypto unavailable this run";
                }
            } else {
                agentEciesPriv_ = eciesPrivStored;                  // legacy plaintext
                if (keyPass && *keyPass)
                    persistSecretConfig("ecies.priv", agentEciesPriv_);   // migrate -> wrapped in place
            }
        }

        // L1: resolve the dedicated enc.priv the SAME way as ecies.priv above (wrapped key
        // decrypts with the passphrase; legacy plaintext is re-wrapped in place when a passphrase
        // is now set; wrapped + missing/wrong passphrase leaves agentEncPriv_ empty + warns).
        if (!encPrivStored.empty()) {
            if (isWrappedSecret(encPrivStored)) {
                if (keyPass && *keyPass) {
                    try { agentEncPriv_ = unwrapSecret(encPrivStored, keyPass); }
                    catch (...) {
                        qWarning() << "[pilot] loadIdentity: enc.priv decrypt FAILED "
                                      "(wrong PILOT_KEY_PASSPHRASE?); A2A crypto unavailable this run";
                    }
                } else {
                    qWarning() << "[pilot] loadIdentity: enc.priv is encrypted but "
                                  "PILOT_KEY_PASSPHRASE is unset; A2A crypto unavailable this run";
                }
            } else {
                agentEncPriv_ = encPrivStored;                      // legacy plaintext
                if (keyPass && *keyPass)
                    persistSecretConfig("enc.priv", agentEncPriv_); // migrate -> wrapped in place
            }
        }

        // L1 migration: a genuinely pre-split DB has NO enc.* rows at all. [FIX-A] Gate the
        // backfill on the RAW STORED slots, never the resolved in-memory key: a wrapped-but-
        // unreadable enc.priv (passphrase missing/wrong) leaves agentEncPriv_ empty but
        // encPrivStored non-empty + agentEncPub_ set from config -> guard is FALSE, so we never
        // regenerate and never overwrite the recoverable wrapped key. The signing identity
        // (ecies.pub == _logos.signing_key) is UNCHANGED, so every peer TOFU pin survives.
        if (db_ && encPrivStored.empty() && agentEncPub_.empty()) {
            ECIESKeypair enc = generateECIESKeypair();
            agentEncPub_  = enc.publicKeyHex;
            agentEncPriv_ = enc.privateKeyHex;
            sqlite3_stmt* es = nullptr;
            sqlite3_prepare_v2(db_,
                "INSERT OR REPLACE INTO config (key, value) VALUES ('enc.pub', ?);", -1, &es, nullptr);
            sqlite3_bind_text(es, 1, agentEncPub_.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(es); sqlite3_finalize(es);
            persistSecretConfig("enc.priv", agentEncPriv_);
        }

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
        // The persisted funding account's keys lived in the wallet that diverged; forget it
        // too, or the next funding would try to spend from an account this wallet cannot sign for.
        sqlite3_exec(db_, "DELETE FROM config WHERE key='funding.public_account';",
                     nullptr, nullptr, nullptr);
    }
    agentAccountId_.clear();
    agentNpk_.clear();
    agentViewingKey_.clear();
}

// Persist the wallet and keep a backup copy of its storage file. This wallet has no
// seed-phrase / private-key export — the storage file IS the only key backup — so we
// keep a second copy that initWallet() can restore from if the main file is lost.
void PilotImpl::backupWallet() {
    if (!isContextReady() || dataDir_.empty()) return;
    modules().lez_core.save();
    std::string storagePath = dataDir_ + "/wallet_storage.json";
    std::ifstream src(storagePath, std::ios::binary);
    if (src.good()) {
        std::ofstream dst(storagePath + ".bak", std::ios::binary | std::ios::trunc);
        dst << src.rdbuf();
    }
}

bool PilotImpl::initWallet() {
    if (walletOpened_) return true;   // idempotent: handle persists in the module process
    if (!isContextReady()) { qWarning() << "[pilot] initWallet: module context not attached"; return false; }

    // Reachability probe (was: poll isConnected() up to 5s). The typed client connects
    // lazily, so ping the wallet BEFORE the open/create ladder: on "module unreachable" we
    // must bail out early — falling through would misread a transport failure as a corrupt
    // wallet file and destructively move it aside in step 3 below.
    {
        logos::CallError perr;
        modules().lez_core.name(&perr, 5000);
        if (!perr.code.empty()) {
            qWarning() << "[pilot] initWallet: wallet module unreachable:"
                       << QString::fromStdString(perr.message);
            return false;
        }
    }

    std::string configPath = dataDir_ + "/wallet_config.json";
    // Storage MUST be a file path (wallet_ffi_open/create_new write a file, not a dir).
    std::string storagePath = dataDir_ + "/wallet_storage.json";
    // LEZ v0.2.2 (lez_core module 549cf115) added a third path to wallet_ffi_open /
    // wallet_ffi_create_new: where the wallet keeps its request statistics. A file path
    // like the storage, alongside it.
    std::string statsPath = dataDir_ + "/wallet_statistics.json";

    std::string sequencerAddr = "http://127.0.0.1:3040";   // local standalone sequencer
    if (const char* env = std::getenv("PILOT_SEQUENCER_ADDR"))
        sequencerAddr = env;

    // Always (re)write the config so the sequencer address is correct for this run.
    {
        std::ofstream cf(configPath, std::ios::trunc);
        if (cf.is_open()) {
            QJsonObject walletCfg;
            // WalletConfig at LEZ v0.2.2 (lez/wallet/src/config.rs) takes a LIST of
            // sequencers — `sequencers: [{sequencer_addr, basic_auth?}]` — in place of the
            // v0.2.0 top-level `sequencer_addr`/`basic_auth`; the old key is unknown to the
            // new struct and `sequencers` is required, so the old shape fails to parse.
            // One entry, no auth (the public testnet is unauthenticated). The
            // multi-sequencer client settings are serde-defaulted and left out.
            QJsonObject seq;
            seq["sequencer_addr"] = QString::fromStdString(sequencerAddr);
            walletCfg["sequencers"] = QJsonArray{seq};
            // Inclusion polling must outlast real-proof block production (RISC0_DEV_MODE=0:
            // blocks carry a real proof and are minutes apart). The old 15-block / 10-retry
            // budget gave up long before a proven transfer landed. Wider budgets are only
            // ceilings — dev-mode blocks are instant, so this doesn't slow the fast path.
            walletCfg["seq_poll_timeout"] = QString("60s");
            walletCfg["seq_tx_poll_max_blocks"] = 60;
            walletCfg["seq_poll_max_retries"] = 60;
            walletCfg["seq_block_poll_max_amount"] = 100;
            // The wallet calibrates every sequencer it has no statistics for with
            // `calibration_limit` sequential latency probes BEFORE open/create returns —
            // 100 by default. Against a local sequencer that is milliseconds; against the
            // public testnet it is 100 internet round-trips (minutes on a slow link) spent
            // measuring the latency of the only sequencer we have. Ten samples are plenty.
            QJsonObject multi;
            multi["distribution_limit"] = 1;
            multi["calibration_limit"] = 10;
            walletCfg["multi_sequencer_client_config"] = multi;
            walletCfg["initial_accounts"] = QJsonArray();
            cf << QJsonDocument(walletCfg).toJson(QJsonDocument::Indented).toStdString();
            cf.close();
        }
    }

    std::string backupPath = storagePath + ".bak";

    // open/create_new are instant against a local sequencer but not against a remote one:
    // measured 2026-08-29 on the public testnet, create_new was still running when its
    // 15 s typed-call budget expired ("create_new result: """ at exactly +15 s), so
    // initialize() returned false on a wallet that was in fact being created. Give both
    // the same budget as a chain sync — the fast path returns the moment the wallet
    // answers, so a local run is not slowed.
    auto tryOpen = [&](const std::string& path) -> bool {
        logos::CallError err;
        int64_t rc = modules().lez_core.open(configPath, path, statsPath, &err, kWalletSyncTimeoutMs);
        return err.code.empty() && rc == 0;
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
    logos::CallError cerr;
    std::string createResult = modules().lez_core.create_new(
        configPath, storagePath, statsPath, walletName, &cerr, kWalletSyncTimeoutMs);
    // NEVER log the body: on success it is the wallet's recovery MNEMONIC (seen verbatim in
    // journald 2026-08-29). Log only whether the call answered and how long the body is.
    qWarning() << "[pilot] initWallet: create_new answered"
               << (cerr.code.empty() ? "ok" : cerr.code.c_str())
               << "body_len=" << createResult.size();
    // Same acceptance as the old QVariant::toInt()==0 check: any non-numeric or "0" body
    // counts as success (atoll of both is 0), an error reply does not.
    if (cerr.code.empty() && std::atoll(createResult.c_str()) == 0) { walletOpened_ = true; return true; }
    return false;
}

// M2: at-rest writer for a secret config value. When PILOT_KEY_PASSPHRASE is set we seal
// the value with wrapSecret (PBKDF2 + AES-256-GCM) so pilot.db never holds the raw key;
// otherwise we keep today's plaintext behavior and warn that pilot.db is key material.
// configKey is ALWAYS a fixed string literal (never peer-controlled), so inlining it in
// the SQL is safe; the VALUE is always bound.
bool PilotImpl::persistSecretConfig(const std::string& configKey, const std::string& clearHex) {
    if (!db_) return false;
    std::string stored = clearHex;
    const char* pass = std::getenv("PILOT_KEY_PASSPHRASE");
    if (pass && *pass) {
        try { stored = wrapSecret(clearHex, pass); }
        catch (...) {
            qWarning() << "[pilot] persistSecretConfig: wrap failed; storing plaintext"
                       << QString::fromStdString(configKey);
        }
    } else {
        qWarning() << "[pilot]" << QString::fromStdString(configKey)
                   << "stored UNENCRYPTED at rest (set PILOT_KEY_PASSPHRASE to encrypt);"
                   << "pilot.db is key material.";
    }
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "INSERT OR REPLACE INTO config (key, value) VALUES ('" + configKey + "', ?);";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, stored.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool PilotImpl::createIdentity() {
    if (!isContextReady()) return false;

    if (!initWallet()) return false;

    std::string accountId = modules().lez_core.create_account_private();
    if (accountId.empty()) return false;

    agentAccountId_ = accountId;

    logos::CallError kerr;
    std::string keysResult = modules().lez_core.get_private_account_keys(agentAccountId_, &kerr);
    if (!kerr.code.empty()) return false;

    agentNpk_ = keysResult;

    QJsonDocument npkDoc = QJsonDocument::fromJson(QString::fromStdString(keysResult).toUtf8());
    if (npkDoc.isObject() && npkDoc.object().contains("viewing_public_key"))
        agentViewingKey_ = npkDoc.object()["viewing_public_key"].toString().toStdString();
    else
        agentViewingKey_ = agentNpk_;

    ECIESKeypair kp = generateECIESKeypair();        // SIGNING identity (== _logos.signing_key); unchanged
    agentEciesPub_ = kp.publicKeyHex;
    agentEciesPriv_ = kp.privateKeyHex;
    // L1: a SEPARATE encryption keypair, independent of the signing identity above. Peers
    // encrypt inbound A2A traffic to this key (advertised as _logos.enc_key); we decrypt with
    // agentEncPriv_. Keeping it distinct means the signing key (the TOFU-pinned identity) is
    // never the ECDH key on new traffic.
    ECIESKeypair enc = generateECIESKeypair();
    agentEncPub_ = enc.publicKeyHex;
    agentEncPriv_ = enc.privateKeyHex;

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

    // M2: ecies.priv is wrapped at rest when PILOT_KEY_PASSPHRASE is set (plaintext
    // otherwise — today's default). ecies.pub above stays plaintext.
    persistSecretConfig("ecies.priv", agentEciesPriv_);

    // L1: the dedicated enc keypair — public key plaintext, private key sealed via the SAME
    // at-rest path as ecies.priv (the enc.priv is the REAL A2A/owner-inbound ECDH key, so it is
    // at least as sensitive as the signing key and must seal identically).
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO config (key, value) VALUES ('enc.pub', ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, agentEncPub_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    persistSecretConfig("enc.priv", agentEncPriv_);

    // Persist the wallet NOW, not only after funding: until a save, the fresh private
    // account exists solely in the wallet module's memory, and fundAgentIfNeeded() is
    // best-effort (dead sequencer / timeout / process kill => backupWallet() never runs).
    // Losing the keys here is what made every subsequent boot's divergence guard reset
    // and re-mint the identity (2026-07-07 identity churn).
    backupWallet();

    return true;
}

// Fund the agent's private account from the dev pinata faucet, once.
// Flow (proven against LEZ v0.1.2): create+register a public account, claim the
// pinata into it (solving the PoW), then shielded-transfer into the agent's
// private account. Idempotent via the 'funded' config flag; best-effort (never
// fatal to startup). NOTE: if the chain is wiped, also clear pilot.db so this re-runs.
bool PilotImpl::fundAgentIfNeeded() {
    if (!isContextReady() || agentAccountId_.empty() || !db_) return false;

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

    // Record WHY funding failed, not just that it did. The module's qWarning output does not
    // reach the daemon log in these runs (measured 2026-07-27: zero [pilot] lines in a 550KB
    // log), so every one of the failure points below was invisible — a funding that died at
    // step 1 looked exactly like one that died at step 4, and the agent carried on reporting
    // nothing was wrong. Persisting the reason makes a funding failure answerable after the
    // fact, from the agent itself, with no log at all.
    auto fundFail = [&](const char* reason) -> bool {
        qWarning() << "[pilot] fund:" << reason;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT OR REPLACE INTO config (key, value) VALUES ('funding.last_error', ?);",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, reason, -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        return false;
    };

    if (!initWallet()) return fundFail("wallet not open");
    if (!isContextReady()) return fundFail("wallet module client unavailable");

    const int64_t fundAmount = 100;   // tokens to move into the agent's private account

    auto syncToHead = [&]() {
        logos::CallError herr;
        int64_t h = modules().lez_core.get_current_block_height(&herr);
        if (herr.code.empty())
            modules().lez_core.sync_to_block(h, nullptr, kWalletSyncTimeoutMs);
    };
    // A transfer-style reply is JSON {"error","success","tx_hash"} in a string; a transport
    // failure comes back as an empty string, which fails the parse — same false as the old
    // null QVariant.
    auto ok = [](const std::string& s) {
        QJsonDocument d = QJsonDocument::fromJson(QString::fromStdString(s).toUtf8());
        return d.isObject() && d.object().value("success").toBool();
    };

    // Chain-wait budget for the three "is it mined yet" loops below. They used to be 60
    // fixed iterations (~18-30 s of wall clock), sized for a local dev sequencer that seals
    // a block every 1-15 s. The public testnet seals one every ~60 s (measured 2026-08-29:
    // 59 / 58 / 63 s between blocks), so a register tx needs two minutes to be two blocks
    // deep and a claim a minute to credit — the fixed count gave up long before either and
    // reported "pinata claim never credited" against a chain that was simply slower. Wait
    // on a wall-clock deadline instead; every loop still exits the moment its condition
    // holds, so a fast local chain is not slowed by a generous budget.
    //   PILOT_CHAIN_WAIT_SECS  budget per wait (default 120 s; the testnet run uses 600)
    //   PILOT_TX_TIMEOUT_MS    RPC timeout of the shielded transfer (default 120 s; with
    //                          RISC0_DEV_MODE=0 the proof is generated inside this call
    //                          and takes far longer — walletSend already allows an hour)
    auto envInt = [](const char* name, int fallback) {
        const char* e = std::getenv(name);
        int v = (e && *e) ? std::atoi(e) : 0;
        return v > 0 ? v : fallback;
    };
    const int waitSecs = envInt("PILOT_CHAIN_WAIT_SECS", 120);
    const int txTimeoutMs = envInt("PILOT_TX_TIMEOUT_MS", 120000);
    auto deadline = [&]() {
        return std::chrono::steady_clock::now() + std::chrono::seconds(waitSecs);
    };

    // 0. Bring a cold wallet to the chain head BEFORE any of the waits below start their
    // clocks. A fresh wallet on the public testnet replays the whole chain inside its
    // first sync (measured 2026-08-29: ~1,000 blocks/min, 29k blocks ≈ 30 min) and every
    // other wallet call queues behind that sync — so the register wait used to burn its
    // entire budget syncing, then the pinata calls timed out one after another. Sync first,
    // on its own budget (PILOT_SYNC_WAIT_SECS, default 1 h), and only then start funding.
    // A synced wallet (local chain, or a second boot) passes through here in one round trip.
    {
        auto until = std::chrono::steady_clock::now()
                   + std::chrono::seconds(envInt("PILOT_SYNC_WAIT_SECS", 3600));
        while (std::chrono::steady_clock::now() < until) {
            logos::CallError herr;
            int64_t head = modules().lez_core.get_current_block_height(&herr);
            if (herr.code.empty()) {
                modules().lez_core.sync_to_block(head, nullptr, kWalletSyncTimeoutMs);
                logos::CallError serr;
                int64_t synced = modules().lez_core.get_last_synced_block(&serr);
                if (serr.code.empty() && synced + 1 >= head) break;   // within a block of head
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    // 1. The public account the faucet pays into. Reuse the one an earlier boot claimed while
    // it still holds the funding amount: every retry used to mint + register + claim a fresh
    // account whenever the shielded step below failed, leaving a trail of funded accounts in
    // the wallet (eight on the public testnet by 2026-09-03, 150 LEZ each). The id is persisted
    // the moment the claim credits — BEFORE the shielded step — so that step failing cannot
    // lose it.
    std::string pubId = fundingPublicAccount();
    bool reused = false;
    if (!pubId.empty()) {
        std::string bal = modules().lez_core.get_balance(pubId, true);
        reused = !bal.empty() && std::atoll(bal.c_str()) >= fundAmount;
        if (reused) {
            qWarning() << "[pilot] fund: reusing funded public account"
                       << QString::fromStdString(pubId) << "balance" << QString::fromStdString(bal);
        } else {
            qWarning() << "[pilot] fund: persisted public account" << QString::fromStdString(pubId)
                       << "holds" << QString::fromStdString(bal) << "- claiming a fresh one";
            pubId.clear();
        }
    }
    if (!reused) {
        // Fresh public account + initialise it on-chain.
        pubId = modules().lez_core.create_account_public();
        if (pubId.empty()) return fundFail("create_account_public failed");
        if (!ok(modules().lez_core.register_public_account(pubId, nullptr, 30000))) {
            return fundFail("register_public_account failed");
        }
        // Wait for the register tx to be mined — claim_pinata requires an initialised
        // recipient on-chain, else the claim is accepted but never credits.
        {
            int64_t start = modules().lez_core.get_current_block_height();
            for (auto until = deadline(); std::chrono::steady_clock::now() < until;) {
                syncToHead();
                logos::CallError herr;
                int64_t h = modules().lez_core.get_current_block_height(&herr);
                if (herr.code.empty() && h >= start + 2) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }

        // 2. Resolve the pinata id, fetch its data, solve the PoW.
        std::string pinataHex = modules().lez_core.account_id_from_base58(kPinataBase58, nullptr, 15000);
        if (pinataHex.empty()) return fundFail("pinata id resolve failed");

        std::string accJson = modules().lez_core.get_account_public(pinataHex, nullptr, 15000);
        QJsonDocument accDoc = QJsonDocument::fromJson(QString::fromStdString(accJson).toUtf8());
        std::string dataHex = accDoc.isObject() ? accDoc.object().value("data").toString().toStdString() : std::string();
        std::string solHex = computePinataSolution(dataHex);
        if (solHex.empty()) return fundFail("pinata data empty or unsolvable — is the faucet present on this chain?");

        // 3. Claim the pinata into the public account.
        if (!ok(modules().lez_core.claim_pinata(pinataHex, pubId, solHex, nullptr, 60000))) {
            return fundFail("claim_pinata failed — faucet may already be claimed on this chain");
        }

        // Wait for the claim tx to be mined and credited before spending it.
        bool credited = false;
        for (auto until = deadline(); !credited && std::chrono::steady_clock::now() < until;) {
            syncToHead();
            std::string bal = modules().lez_core.get_balance(pubId, true);
            if (!bal.empty() && std::atoll(bal.c_str()) >= fundAmount) credited = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        if (!credited) return fundFail("pinata claim never credited the public account");

        // The claim credited: remember this account NOW, before the shielded step, and save
        // the wallet so its keys outlive a crash during the proof. From here on this is the
        // account the public spend rail (wallet.send to public:<id>) pays from.
        sqlite3_stmt* ps = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT OR REPLACE INTO config (key, value) VALUES ('funding.public_account', ?);",
                -1, &ps, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(ps, 1, pubId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(ps);
            sqlite3_finalize(ps);
        }
        backupWallet();
    }

    // 4. Shielded transfer public -> agent's private account (generates a ZK proof).
    if (!ok(modules().lez_core.transfer_shielded_owned(
                pubId, agentAccountId_, u128LeHex(fundAmount), nullptr, txTimeoutMs))) {
        return fundFail("transfer_shielded_owned failed");
    }
    syncToHead();

    // 4b. VERIFY the money actually landed in the agent's private account before claiming it.
    // The shielded transfer returning success only means the wallet accepted the request; the
    // notes still have to be mined and seen. Step 3 above already waits for the pinata claim to
    // credit before spending it — this step was the one place that took an RPC's word for it and
    // wrote funded=1 regardless. That is how an agent ends up reporting funded=1 with nothing
    // spendable, which is unfalsifiable from the outside and wasted a diagnosis cycle.
    bool landed = false;
    for (auto until = deadline(); !landed && std::chrono::steady_clock::now() < until;) {
        syncToHead();
        std::string bal = modules().lez_core.get_balance(agentAccountId_, false);
        if (!bal.empty() && std::atoll(bal.c_str()) >= fundAmount) landed = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    if (!landed) {
        // Do NOT mark funded. Leaving the flag clear means the next boot retries funding
        // instead of inheriting a false claim that nothing can disprove.
        return fundFail("shielded transfer accepted but the funds never appeared in the "
                        "agent's private account");
    }

    // 5. Mark funded.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO config (key, value) VALUES ('funded', '1');",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    // Funding succeeded — clear any recorded reason from an earlier failed attempt so a
    // stale message can never be mistaken for the current state.
    sqlite3_exec(db_, "DELETE FROM config WHERE key='funding.last_error';",
                 nullptr, nullptr, nullptr);
    qWarning() << "[pilot] fund: agent private account funded with" << fundAmount;
    backupWallet();   // persist + keep a recovery copy of the now-funded wallet
    return true;
}

std::string PilotImpl::fundingPublicAccount() {
    if (!db_) return std::string();
    std::string id;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM config WHERE key='funding.public_account';",
                           -1, &s, nullptr) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_text(s, 0))
            id = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
        sqlite3_finalize(s);
    }
    return id;
}

std::string PilotImpl::getAgentNpk() {
    return agentNpk_;
}

std::string PilotImpl::getAccountId() {
    return agentAccountId_;
}

std::string PilotImpl::walletBalance() {
    if (!isContextReady() || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";

    // Sync to chain head so the balance reflects the latest blocks.
    //
    // The timeout must outlast a real catch-up, not a hoped-for one: a measured sync of a
    // ~3500-block chain took 37.2s ("Synced to block 3500 in 37.210232932s") against the old
    // 30s cap, so the sync was being CUT OFF mid-flight on every chain of any age. A truncated
    // sync leaves the wallet believing it is further behind than it is, which is how a balance
    // read and a spend end up disagreeing about the same account.
    logos::CallError herr;
    int64_t head = modules().lez_core.get_current_block_height(&herr);
    if (herr.code.empty())
        modules().lez_core.sync_to_block(head, nullptr, kWalletSyncTimeoutMs);

    logos::CallError berr;
    std::string result = modules().lez_core.get_balance(agentAccountId_, false, &berr);

    if (!berr.code.empty())
        return "{\"error\": \"balance query failed\"}";

    QJsonObject obj;
    obj["balance"] = QString::fromStdString(result);
    obj["account"] = QString::fromStdString(agentAccountId_);
    // The public account funding claimed the faucet into, once one is persisted: the balance
    // the public spend rail (wallet.send to public:<id>) pays from. On the public testnet this
    // is where the agent's money actually is until a real shielded proof moves it private.
    std::string pub = fundingPublicAccount();
    if (!pub.empty()) {
        obj["public_account"] = QString::fromStdString(pub);
        logos::CallError perr;
        std::string pbal = modules().lez_core.get_balance(pub, true, &perr);
        obj["public_balance"] = perr.code.empty() ? QString::fromStdString(pbal)
                                                  : QString("unavailable");
    }
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::walletHistory() {
    if (!isContextReady() || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";
    if (!db_) return "{\"error\": \"database not open\"}";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT id, recipient, amount, state, created_at, tx_hash, error FROM spend_requests "
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
        // Settlement evidence and, for a failed spend, why (both '' when not applicable).
        if (const unsigned char* h = sqlite3_column_text(stmt, 5))
            tx["tx_hash"] = QString(reinterpret_cast<const char*>(h));
        if (const unsigned char* e = sqlite3_column_text(stmt, 6))
            tx["error"] = QString(reinterpret_cast<const char*>(e));
        arr.append(tx);
    }
    sqlite3_finalize(stmt);

    QJsonObject root;
    root["transactions"] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}
