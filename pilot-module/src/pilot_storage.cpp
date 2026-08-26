#include "pilot_impl.h"
#include "pilot_crypto.h"
// Generated per-build; typed clients for storage_module + delivery_module (see pilot_impl.h).
#include "logos_sdk.h"
#include <sqlite3.h>
#include <fstream>
#include <chrono>
#include <thread>
#include <cstring>
#include <QString>
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

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

    StdLogosResult initResult = modules().storage_module.uploadInit(label, 65536);
    std::string sessionId;
    if (initResult.success && initResult.value.is_string())
        sessionId = initResult.value.get<std::string>();
    if (sessionId.empty())
        return "{\"error\": \"upload init failed\"}";

    // The lp transport marshals arguments as JSON, and JSON strings must be valid UTF-8 —
    // raw AES ciphertext cannot ride in the chunk argument. Base64-wrap it here; the
    // download path (storageDownload) unwraps symmetrically. Both ends are pilot code, so
    // the stored object's format is ours to define.
    std::string chunkB64 = QByteArray(reinterpret_cast<const char*>(encrypted.data()),
                                      static_cast<int>(encrypted.size()))
                               .toBase64().toStdString();
    modules().storage_module.uploadChunk(sessionId, chunkB64);

    StdLogosResult finalResult = modules().storage_module.uploadFinalize(sessionId);
    std::string cid;
    if (finalResult.success && finalResult.value.is_string())
        cid = finalResult.value.get<std::string>();
    if (cid.empty())
        return "{\"error\": \"upload finalize failed\"}";

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

    std::string tmpPath = path + ".enc";
    // The old four-arg downloadChunks(cid, local, chunkSize, outPath) is now split upstream:
    // downloadToUrl is the write-to-file variant (downloadChunks lost its path and streams
    // chunks via events instead).
    StdLogosResult result = modules().storage_module.downloadToUrl(cid, tmpPath, false, 65536);
    if (!result.success) {
        QJsonObject err;
        err["error"] = QString::fromStdString("download failed: " + result.error);
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }

    // Poll for async download to complete (max 30s)
    for (int i = 0; i < 60; i++) {
        std::ifstream check(tmpPath, std::ios::binary | std::ios::ate);
        if (check.is_open() && check.tellg() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::ifstream encFile(tmpPath, std::ios::binary);
    if (!encFile.is_open())
        return "{\"error\": \"download timed out — file not received from network\"}";
    std::string encContent((std::istreambuf_iterator<char>(encFile)),
                            std::istreambuf_iterator<char>());
    encFile.close();
    std::remove(tmpPath.c_str());

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
