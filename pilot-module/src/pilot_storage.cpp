#include "pilot_impl.h"
#include "pilot_crypto.h"
// Generated per-build; typed clients for storage_module + delivery_module (see pilot_impl.h).
#include "logos_sdk.h"
#include <sqlite3.h>
#include <fstream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <set>
#include <cstring>
#include <QString>
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>

// ---- REST transport (2026-08-28) ----------------------------------------------------------
// Uploads and downloads talk to libstorage's own REST API on loopback instead of the typed
// client. Why is on pilotStorageApiPort() in pilot_impl.h: the storage host's first event
// emission ("storageStart", forwarded from its FFI thread) breaks the QtRO channel's socket
// notifier, and every reply after it is dropped — uploadInit's answer included, so the old
// three-call upload hung forever on a session id that had already been created. Same upstream
// host bug, same shape of workaround, as the delivery pull path in pilot_a2a.cpp.
namespace {

std::string storageRestBase() {
    return "http://127.0.0.1:" + std::to_string(pilotStorageApiPort()) + "/api/storage/v1";
}

// Blocking HTTP with a hard deadline, on the same nested-QEventLoop pattern as the delivery
// pull path's httpGetBody. verb is "GET" or "POST"; *httpStatus is 0 when nothing came back.
std::string storageRest(const char* verb, const std::string& url, const QByteArray& body,
                        int timeoutMs, int* httpStatus) {
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(QString::fromStdString(url))};
    QNetworkReply* reply = nullptr;
    if (std::strcmp(verb, "POST") == 0) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
        reply = manager.post(request, body);
    } else {
        reply = manager.get(request);
    }
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    deadline.start(timeoutMs);
    loop.exec();
    std::string out;
    int status = 0;
    if (reply->isFinished()) {
        status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        out = reply->readAll().toStdString();
    } else {
        reply->abort();
    }
    if (httpStatus) *httpStatus = status;
    reply->deleteLater();
    return out;
}

// The manifest CIDs the node's repo holds on disk. libstorage keys each stored manifest by
// its CID: <data-dir>/repo/manifests/<xx>/<cid>.dsobj (measured 2026-08-28: the upload's
// "Stored data manifestCid=zDv*HKVQZT" log line matched a file named
// zDvZRwzm…szHKVQZT.dsobj). The repo is pilot's own data dir, so this is a side channel the
// broken host reply path cannot corrupt — the delivery pull path's idea, applied to storage.
std::set<std::string> storageManifestCids(const std::string& repoDir) {
    std::set<std::string> out;
    std::error_code ec;
    std::filesystem::path root = std::filesystem::path(repoDir) / "manifests";
    if (!std::filesystem::is_directory(root, ec)) return out;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string name = it->path().filename().string();
        auto dot = name.find('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        if (!name.empty()) out.insert(name);
    }
    return out;
}

// Is the REST API answering? Callers pass deadline 0 (a single probe): this module build
// never opens the port, and every 250 ms spent here counts against the CLI's ~10 s RPC
// window that the whole upload/download must fit inside.
bool storageRestReady(int deadlineMs) {
    const std::string url = storageRestBase() + "/debug/info";
    for (int waited = 0; waited <= deadlineMs; waited += 250) {
        int status = 0;
        storageRest("GET", url, {}, 2000, &status);
        if (status == 200) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

}  // namespace

void PilotImpl::startStorageNodeIfNeeded() {
    if (storageNodeStarted_) return;
    storageNodeStarted_ = true;
    // From this point the host's reply channel is poisoned (storageStart's emit) — every
    // typed storage call AFTER this start loses its reply until the host restarts. Uploads
    // therefore happen before the first start; the REST path, when a build serves it, has
    // no such limit.
    modules().storage_module.start(nullptr, 10000);
    qWarning() << "[pilot] storage node started (announcing)";
}

static std::string currentTs() {
    auto now = std::chrono::system_clock::now();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
}

std::string PilotImpl::storageUpload(const std::string& path, const std::string& label) {
    if (!isContextReady() || !db_) return "{\"error\": \"not initialized\"}";
    initStorageModule();

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        QJsonObject err;
        err["error"] = QString::fromStdString("cannot open file: " + path);
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    AESKey fileKey = generateFileKey();
    std::vector<uint8_t> plainBytes(content.begin(), content.end());
    std::vector<uint8_t> encrypted = aesEncrypt(plainBytes, fileKey);

    // The stored object stays base64-wrapped ciphertext (the download path unwraps
    // symmetrically; both ends are pilot code, so the format is ours to define).
    QByteArray chunkB64 = QByteArray(reinterpret_cast<const char*>(encrypted.data()),
                                     static_cast<int>(encrypted.size()))
                              .toBase64();

    std::string cid;
    if (storageRestReady(0)) {
        // One POST replaces uploadInit/uploadChunk/uploadFinalize: the response body is the
        // CID — as bare text, but accept a {"cid": …} JSON shape too. (This module build's
        // FFI path does not start the API server — measured 2026-08-28, config accepted but
        // the port never opens — so this branch waits for a build that does.)
        int status = 0;
        cid = storageRest("POST", storageRestBase() + "/data", chunkB64, 60000, &status);
        if (!cid.empty() && cid.front() == '{') {
            QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(cid));
            cid = d.isObject() ? d.object().value("cid").toString().toStdString() : "";
        }
        while (!cid.empty() && (cid.back() == '\n' || cid.back() == '\r' || cid.back() == '"'))
            cid.pop_back();
        if (!cid.empty() && cid.front() == '"') cid.erase(cid.begin());
        if (status != 200) cid.clear();
    }
    if (cid.empty()) {
        // Typed-client path, measured end to end on 2026-08-28 (journald + the node's own
        // log): with the node not yet started, uploadInit and uploadChunk both get their
        // replies; the chunk's storageUploadProgress event is the host's FIRST emit and it
        // poisons the channel, so uploadFinalize EXECUTES module-side ("Stored data
        // manifestCid=…" in storage.log) but its reply never arrives. The CID is therefore
        // read from the repo on disk, where the node keys the new manifest by it. No retry:
        // a second uploadInit on the poisoned channel only burns 20 s.
        const std::string repoDir = dataDir_ + "/storage/repo";
        StdLogosResult initResult = modules().storage_module.uploadInit(label, 65536);
        std::string sessionId;
        if (initResult.success && initResult.value.is_string())
            sessionId = initResult.value.get<std::string>();
        if (sessionId.empty())
            return "{\"error\": \"upload init failed\"}";
        // The chunk's reply races the storageUploadProgress emit (measured both ways within
        // 2 ms of each other); the data lands either way, so do not wait 20 s for it.
        std::set<std::string> before = storageManifestCids(repoDir);
        modules().storage_module.uploadChunk(sessionId, chunkB64.toStdString(), nullptr, 3000);
        // Short timeout: the reply is expected to be lost; the manifest lands within ~50 ms.
        StdLogosResult finalResult = modules().storage_module.uploadFinalize(sessionId, nullptr, 3000);
        if (finalResult.success && finalResult.value.is_string())
            cid = finalResult.value.get<std::string>();
        for (int i = 0; cid.empty() && i < 20; i++) {
            for (const auto& c : storageManifestCids(repoDir))
                if (!before.count(c)) { cid = c; break; }
            if (cid.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (cid.empty())
            return "{\"error\": \"upload finalized but no new manifest appeared in the repo\"}";
    }

    // Announce what we just stored — this is the first (and only) start of the node, done
    // AFTER the upload so the typed calls above ran on the un-poisoned pre-start channel.
    startStorageNodeIfNeeded();

    std::string keyHex = aesKeyToHex(fileKey);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO stored_files (cid, label, file_key_encrypted, timestamp, size_bytes) VALUES (?, ?, ?, ?, ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, cid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, keyHex.c_str(), -1, SQLITE_TRANSIENT);
    std::string ts = currentTs();
    sqlite3_bind_text(stmt, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(content.size()));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    QJsonObject result;
    result["cid"] = QString::fromStdString(cid);
    result["label"] = QString::fromStdString(label);
    result["encrypted"] = true;
    return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::storageDownload(const std::string& cid, const std::string& path) {
    if (!isContextReady() || !db_) return "{\"error\": \"not initialized\"}";
    initStorageModule();

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT file_key_encrypted FROM stored_files WHERE cid = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, cid.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return "{\"error\": \"unknown CID\"}";
    }
    std::string keyHex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    // REST first (local repo, then the storage network for a CID a peer shared with us),
    // typed client as the fallback — same two-world reasoning as storageUpload above.
    std::string encContent;
    bool haveContent = false;
    if (storageRestReady(0)) {
        int status = 0;
        encContent = storageRest("GET", storageRestBase() + "/data/" + cid, {}, 30000, &status);
        if (status != 200) {
            status = 0;
            encContent = storageRest(
                "GET", storageRestBase() + "/data/" + cid + "/network/stream", {}, 120000,
                &status);
        }
        haveContent = (status == 200);
    }
    if (!haveContent) {
        // A remote CID needs the node on the network. This may be the first start (fine) or
        // a later one (no-op); either way downloadToUrl's REPLY may be lost to the post-start
        // poisoned channel — so its status is ignored and the FILE is the truth: the module
        // writes it asynchronously regardless of whether the reply got back to us.
        startStorageNodeIfNeeded();
        std::string tmpPath = path + ".enc";
        // The old four-arg downloadChunks(cid, local, chunkSize, outPath) is now split
        // upstream: downloadToUrl is the write-to-file variant.
        // Short timeout: post-start the reply is lost every time, but the module has the
        // file on disk within ~60 ms (measured: storageDownloadDone 58 ms after the call).
        modules().storage_module.downloadToUrl(cid, tmpPath, false, 65536, nullptr, 3000);
        for (int i = 0; i < 60; i++) {
            std::ifstream check(tmpPath, std::ios::binary | std::ios::ate);
            if (check.is_open() && check.tellg() > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        std::ifstream encFile(tmpPath, std::ios::binary);
        if (!encFile.is_open())
            return "{\"error\": \"download timed out — file not received from network\"}";
        encContent.assign((std::istreambuf_iterator<char>(encFile)),
                          std::istreambuf_iterator<char>());
        encFile.close();
        std::remove(tmpPath.c_str());
    }

    AESKey fileKey = aesKeyFromHex(keyHex);
    // Uploads are base64-wrapped (see storageUpload) — unwrap before decrypting.
    QByteArray encRaw = QByteArray::fromBase64(QByteArray::fromStdString(encContent));
    std::vector<uint8_t> encBytes(encRaw.begin(), encRaw.end());
    if (encBytes.empty())
        return "{\"error\": \"download failed — empty file received\"}";

    std::vector<uint8_t> decrypted;
    try {
        decrypted = aesDecrypt(encBytes, fileKey);
    } catch (const std::exception& e) {
        QJsonObject err;
        err["error"] = QString::fromStdString(std::string("decryption failed: ") + e.what());
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }

    std::ofstream outFile(path, std::ios::binary);
    if (!outFile.is_open())
        return "{\"error\": \"cannot write output file\"}";
    outFile.write(reinterpret_cast<const char*>(decrypted.data()),
                  static_cast<std::streamsize>(decrypted.size()));
    outFile.close();

    QJsonObject res;
    res["path"] = QString::fromStdString(path);
    res["cid"] = QString::fromStdString(cid);
    res["decrypted"] = true;
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::storageList() {
    if (!db_) return "{\"error\": \"not initialized\"}";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT cid, label, timestamp FROM stored_files ORDER BY timestamp DESC;",
        -1, &stmt, nullptr);

    QJsonArray arr;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QJsonObject obj;
        obj["cid"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        obj["label"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        obj["timestamp"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        arr.append(obj);
    }
    sqlite3_finalize(stmt);

    QJsonObject root;
    root["files"] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::storageShare(const std::string& cid, const std::string& recipientNpk) {
    if (!isContextReady() || !db_) return "{\"error\": \"not initialized\"}";
    initStorageModule();

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT file_key_encrypted FROM stored_files WHERE cid = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, cid.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return "{\"error\": \"unknown CID\"}";
    }
    std::string keyHex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    std::string recipientKey = recipientNpk;
    QJsonDocument recipientDoc = QJsonDocument::fromJson(QByteArray::fromStdString(recipientNpk));
    if (recipientDoc.isObject() && recipientDoc.object().contains("viewing_public_key"))
        recipientKey = recipientDoc.object()["viewing_public_key"].toString().toStdString();

    QJsonObject shareObj;
    shareObj["cid"] = QString::fromStdString(cid);
    shareObj["key"] = QString::fromStdString(keyHex);
    std::string sharePayload = QJsonDocument(shareObj).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> plainBytes(sharePayload.begin(), sharePayload.end());

    std::string encPayload;
    try {
        ECIESCiphertext encrypted = eciesEncrypt(recipientKey, plainBytes);
        encPayload = eciesSerialize(encrypted);
    } catch (const std::exception& e) {
        QJsonObject err;
        err["error"] = QString::fromStdString(std::string("encryption failed: ") + e.what());
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }

    std::string topic = "/pilot/1/inbox-" + recipientNpk + "/proto";

    modules().delivery_module.send(
        topic, std::vector<uint8_t>(encPayload.begin(), encPayload.end()), nullptr, kDeliveryFireAndForgetMs);

    QJsonObject result;
    result["shared"] = true;
    result["cid"] = QString::fromStdString(cid);
    result["recipient"] = QString::fromStdString(recipientNpk);
    result["encrypted"] = true;
    return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
}
