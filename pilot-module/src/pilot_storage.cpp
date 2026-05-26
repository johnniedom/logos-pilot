#include "pilot_impl.h"
#include "pilot_crypto.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include <sqlite3.h>
#include <fstream>
#include <chrono>
#include <cstring>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QByteArray>
#include "logos_types.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

static std::string currentTs() {
    auto now = std::chrono::system_clock::now();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
}

std::string PilotImpl::storageUpload(const std::string& path, const std::string& label) {
    if (!logosAPI_ || !db_) return "{\"error\": \"not initialized\"}";

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

    auto* storage = logosAPI_->getClient("storage_module");
    if (!storage) return "{\"error\": \"storage module unavailable\"}";

    QVariant initResult = storage->invokeRemoteMethod(
        "storage_module", "uploadInit",
        QString::fromStdString(label));

    QString sessionId;
    if (initResult.canConvert<LogosResult>()) {
        LogosResult lr = initResult.value<LogosResult>();
        if (lr.success)
            sessionId = lr.value.toString();
    } else if (initResult.canConvert<QVariantMap>()) {
        sessionId = initResult.toMap().value("value").toString();
    } else {
        sessionId = initResult.toString();
    }
    if (sessionId.isEmpty())
        return "{\"error\": \"upload init failed\"}";

    QByteArray chunk(reinterpret_cast<const char*>(encrypted.data()),
                     static_cast<int>(encrypted.size()));
    storage->invokeRemoteMethod(
        "storage_module", "uploadChunk",
        sessionId, QVariant(chunk));

    QVariant finalResult = storage->invokeRemoteMethod(
        "storage_module", "uploadFinalize", sessionId);

    std::string cid;
    if (finalResult.canConvert<LogosResult>()) {
        LogosResult lr = finalResult.value<LogosResult>();
        if (lr.success)
            cid = lr.value.toString().toStdString();
    } else if (finalResult.canConvert<QVariantMap>()) {
        cid = finalResult.toMap().value("value").toString().toStdString();
    } else {
        cid = finalResult.toString().toStdString();
    }
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

    QJsonObject result;
    result["cid"] = QString::fromStdString(cid);
    result["label"] = QString::fromStdString(label);
    result["encrypted"] = true;
    return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
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
        "storage_module", "downloadChunks",
        QString::fromStdString(cid),
        QVariant(false),
        QVariant(65536),
        QString::fromStdString(tmpPath));

    std::string downloadedData;
    if (result.canConvert<LogosResult>()) {
        LogosResult lr = result.value<LogosResult>();
        if (!lr.success) {
            QJsonObject err;
            err["error"] = QString::fromStdString("download failed: " + lr.error.toString().toStdString());
            return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
        }
        downloadedData = lr.value.toString().toStdString();
    }

    std::ifstream encFile(tmpPath, std::ios::binary);
    if (!encFile.is_open() && downloadedData.empty())
        return "{\"error\": \"download failed: no data returned\"}";
    std::string encContent((std::istreambuf_iterator<char>(encFile)),
                            std::istreambuf_iterator<char>());
    encFile.close();
    std::remove(tmpPath.c_str());

    AESKey fileKey = aesKeyFromHex(keyHex);
    std::vector<uint8_t> encBytes(encContent.begin(), encContent.end());
    if (encBytes.empty()) {
        QJsonObject res;
        res["status"] = QString("downloading");
        res["cid"] = QString::fromStdString(cid);
        res["note"] = QString("download is async — file will be available via storageDownloadDone event");
        return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
    }

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

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery) return "{\"error\": \"delivery module unavailable\"}";

    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(topic),
        QString::fromStdString(encPayload));

    QJsonObject result;
    result["shared"] = true;
    result["cid"] = QString::fromStdString(cid);
    result["recipient"] = QString::fromStdString(recipientNpk);
    result["encrypted"] = true;
    return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
}
