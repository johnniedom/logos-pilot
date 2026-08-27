#include <logos_test.h>
#include "../src/pilot_impl.h"
#include <string>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>

// The JSON pilot hands to storage_module.init is parsed by libstorage (confutils against
// StorageConf); anything it cannot parse is a ConfigurationError the library reports only as
// "unable to load configuration", and the module then answers every upload with
// "upload init failed". That is how every fresh agent's storage died through 2026-08-27:
// the config was {"nat":"none"} and this libstorage's `nat` takes only `auto` or
// `extip:<IP>`. These tests pin the shape libstorage was measured to accept.

static QJsonObject parseCfg(const std::string& s) {
    QJsonDocument d = QJsonDocument::fromJson(QString::fromStdString(s).toUtf8());
    return d.isObject() ? d.object() : QJsonObject();
}

static std::string scratchDir(const char* tag) {
    std::string d = std::filesystem::temp_directory_path().string() + "/pilot-stgcfg-" + tag +
                    "-" + std::to_string(static_cast<long long>(getpid()));
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

LOGOS_TEST(storage_config_gives_the_node_a_repo_under_the_agent_data_dir) {
    std::string dir = scratchDir("repo");
    unsetenv("PILOT_STORAGE_NAT");
    QJsonObject cfg = parseCfg(pilotStorageInitConfig(dir));
    LOGOS_ASSERT_EQ(cfg.value("data-dir").toString().toStdString(), dir + "/storage");
    LOGOS_ASSERT_EQ(cfg.value("log-file").toString().toStdString(), dir + "/storage/storage.log");
    LOGOS_ASSERT_TRUE(cfg.contains("log-level"));
    // libstorage opens exactly the path it is given and does not create it.
    LOGOS_ASSERT_TRUE(std::filesystem::is_directory(dir + "/storage"));
    std::filesystem::remove_all(dir);
}

LOGOS_TEST(storage_config_never_sends_a_nat_value_libstorage_rejects) {
    std::string dir = scratchDir("nat");
    unsetenv("PILOT_STORAGE_NAT");
    QJsonObject cfg = parseCfg(pilotStorageInitConfig(dir));
    // No nat key at all: libstorage's default is `auto`. "none" is the value that broke init.
    LOGOS_ASSERT_FALSE(cfg.contains("nat"));
    LOGOS_ASSERT_TRUE(pilotStorageInitConfig(dir).find("\"none\"") == std::string::npos);
    std::filesystem::remove_all(dir);
}

LOGOS_TEST(storage_config_honours_an_explicit_nat_override) {
    std::string dir = scratchDir("extip");
    setenv("PILOT_STORAGE_NAT", "extip:127.0.0.1", 1);
    QJsonObject cfg = parseCfg(pilotStorageInitConfig(dir));
    unsetenv("PILOT_STORAGE_NAT");
    LOGOS_ASSERT_EQ(cfg.value("nat").toString().toStdString(), std::string("extip:127.0.0.1"));
    std::filesystem::remove_all(dir);
}

LOGOS_TEST(storage_config_without_a_data_dir_is_still_valid_json) {
    // Before initialize() there is no data dir; the config must then be the empty object,
    // which libstorage accepts (measured: {} -> init true), never a repo at "/storage".
    unsetenv("PILOT_STORAGE_NAT");
    std::string s = pilotStorageInitConfig("");
    QJsonObject cfg = parseCfg(s);
    LOGOS_ASSERT_TRUE(cfg.isEmpty());
    LOGOS_ASSERT_EQ(s, std::string("{}"));
}
