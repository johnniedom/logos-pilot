#include "pilot_impl.h"
#include "pilot_crypto.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include <sqlite3.h>
#include <sstream>
#include <fstream>
#include <chrono>
#include <cstring>
#include <QString>
#include <QVariant>
#include <QByteArray>

static std::string currentTs() {
    auto now = std::chrono::system_clock::now();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
}

std::string PilotImpl::storageUpload(const std::string& path, const std::string& label) {
    if (!logosAPI_ || !db_) return "{\"error\": \"not initialized\"}";

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return "{\"error\": \"cannot open file: " + path + "\"}";

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    AESKey fileKey = generateFileKey();
    std::vector<uint8_t> plainBytes(content.begin(), content.end());
    std::vector<uint8_t> encrypted = aesEncrypt(plainBytes, fileKey);

    auto* storage = logosAPI_->getClient("storage_module");
    if (!storage) return "{\"error\": \"storage module unavailable\"}";

    QVariant initResult = storage->invokeRemoteMethod(
        "storage_module", "uploadInit",
        QString::fromStdString(label));

    QString sessionId = initResult.toString();
    if (sessionId.isEmpty())
        return "{\"error\": \"upload init failed\"}";

    QByteArray chunk(reinterpret_cast<const char*>(encrypted.data()),
                     static_cast<int>(encrypted.size()));
    storage->invokeRemoteMethod(
        "storage_module", "uploadChunk",
        sessionId, QVariant(chunk));

    QVariant finalResult = storage->invokeRemoteMethod(
        "storage_module", "uploadFinalize", sessionId);

    std::string cid = finalResult.toString().toStdString();
    if (cid.empty())
        return "{\"error\": \"upload finalize failed\"}";

    std::string keyHex = aesKeyToHex(fileKey);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO stored_files (cid, label, file_key_encrypted, timestamp) VALUES (?, ?, ?, ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, cid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, keyHex.c_str(), -1, SQLITE_TRANSIENT);
    std::string ts = currentTs();
    sqlite3_bind_text(stmt, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return "{\"cid\": \"" + cid + "\", \"label\": \"" + label + "\", \"encrypted\": true}";
}

std::string PilotImpl::storageDownload(const std::string& cid, const std::string& path) {
    if (!logosAPI_ || !db_) return "{\"error\": \"not initialized\"}";

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

    auto* storage = logosAPI_->getClient("storage_module");
    if (!storage) return "{\"error\": \"storage module unavailable\"}";

    std::string tmpPath = path + ".enc";
    QVariant result = storage->invokeRemoteMethod(
        "storage_module", "downloadFile",
        QString::fromStdString(cid),
        QString::fromStdString(tmpPath),
        QVariant(0));

    if (result.isNull())
        return "{\"error\": \"download failed\"}";

    std::ifstream encFile(tmpPath, std::ios::binary);
    if (!encFile.is_open())
        return "{\"error\": \"cannot read downloaded file\"}";
    std::string encContent((std::istreambuf_iterator<char>(encFile)),
                            std::istreambuf_iterator<char>());
    encFile.close();
    std::remove(tmpPath.c_str());

    AESKey fileKey = aesKeyFromHex(keyHex);
    std::vector<uint8_t> encBytes(encContent.begin(), encContent.end());
    std::vector<uint8_t> decrypted = aesDecrypt(encBytes, fileKey);

    std::ofstream outFile(path, std::ios::binary);
    if (!outFile.is_open())
        return "{\"error\": \"cannot write output file\"}";
    outFile.write(reinterpret_cast<const char*>(decrypted.data()),
                  static_cast<std::streamsize>(decrypted.size()));
    outFile.close();

    return "{\"path\": \"" + path + "\", \"cid\": \"" + cid + "\", \"decrypted\": true}";
}

std::string PilotImpl::storageList() {
    if (!db_) return "{\"error\": \"not initialized\"}";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT cid, label, timestamp FROM stored_files ORDER BY timestamp DESC;",
        -1, &stmt, nullptr);

    std::ostringstream json;
    json << "{\"files\": [";
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) json << ",";
        first = false;
        json << "{"
             << "\"cid\": \"" << sqlite3_column_text(stmt, 0) << "\","
             << "\"label\": \"" << sqlite3_column_text(stmt, 1) << "\","
             << "\"timestamp\": \"" << sqlite3_column_text(stmt, 2) << "\""
             << "}";
    }
    json << "]}";
    sqlite3_finalize(stmt);

    return json.str();
}

std::string PilotImpl::storageShare(const std::string& cid, const std::string& recipientNpk) {
    if (!logosAPI_ || !db_) return "{\"error\": \"not initialized\"}";

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

    std::string sharePayload = "{\"cid\": \"" + cid + "\", \"key\": \"" + keyHex + "\"}";
    std::vector<uint8_t> plainBytes(sharePayload.begin(), sharePayload.end());
    ECIESCiphertext encrypted = eciesEncrypt(recipientNpk, plainBytes);
    std::string encPayload = eciesSerialize(encrypted);

    std::string topic = "/pilot/1/inbox-" + recipientNpk + "/proto";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery) return "{\"error\": \"delivery module unavailable\"}";

    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(topic),
        QString::fromStdString(encPayload));

    return "{\"shared\": true, \"cid\": \"" + cid + "\", \"recipient\": \"" + recipientNpk + "\", \"encrypted\": true}";
}
