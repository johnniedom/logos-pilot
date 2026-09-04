#include <logos_test.h>
#include "../src/pilot_impl.h"
#include <string>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
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
    unsetenv("PILOT_STORAGE_API_PORT");
    QJsonObject cfg = parseCfg(pilotStorageInitConfig(dir));
    LOGOS_ASSERT_EQ(cfg.value("data-dir").toString().toStdString(), dir + "/storage");
    LOGOS_ASSERT_EQ(cfg.value("log-file").toString().toStdString(), dir + "/storage/storage.log");
    LOGOS_ASSERT_TRUE(cfg.contains("log-level"));
    // libstorage opens exactly the path it is given and does not create it.
    LOGOS_ASSERT_TRUE(std::filesystem::is_directory(dir + "/storage"));
    std::filesystem::remove_all(dir);
}

LOGOS_TEST(storage_config_turns_on_the_loopback_rest_api_uploads_ride) {
    // Upload/download go through libstorage's REST API, not the typed client (whose replies
    // the host drops after its first event emit — see pilotStorageApiPort in pilot_impl.h).
    // The config must therefore always open the API, and only on loopback: it has no auth.
    std::string dir = scratchDir("rest");
    unsetenv("PILOT_STORAGE_NAT");
    unsetenv("PILOT_STORAGE_API_PORT");
    QJsonObject cfg = parseCfg(pilotStorageInitConfig(dir));
    LOGOS_ASSERT_EQ(cfg.value("api-bindaddr").toString().toStdString(), std::string("127.0.0.1"));
    LOGOS_ASSERT_EQ(cfg.value("api-port").toInt(), pilotStorageApiPort());
    LOGOS_ASSERT_TRUE(pilotStorageApiPort() > 0);
    std::filesystem::remove_all(dir);
}

LOGOS_TEST(storage_config_honours_the_api_port_override_for_multi_agent_hosts) {
    // Two agents on one host each need their own node API; the env var keeps them apart.
    std::string dir = scratchDir("port");
    setenv("PILOT_STORAGE_API_PORT", "6021", 1);
    QJsonObject cfg = parseCfg(pilotStorageInitConfig(dir));
    unsetenv("PILOT_STORAGE_API_PORT");
    LOGOS_ASSERT_EQ(cfg.value("api-port").toInt(), 6021);
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

LOGOS_TEST(storage_config_honours_the_disc_port_override_for_multi_agent_hosts) {
    // libstorage's discovery (UDP) port defaults to 8090 for every node. Two agents on one
    // host — the storage-role job runs two daemons in one runner — need distinct ports or the
    // second node cannot bind.
    std::string dir = scratchDir("disc");
    unsetenv("PILOT_STORAGE_DISC_PORT");
    LOGOS_ASSERT_FALSE(parseCfg(pilotStorageInitConfig(dir)).contains("disc-port"));
    setenv("PILOT_STORAGE_DISC_PORT", "8091", 1);
    QJsonObject cfg = parseCfg(pilotStorageInitConfig(dir));
    unsetenv("PILOT_STORAGE_DISC_PORT");
    LOGOS_ASSERT_EQ(cfg.value("disc-port").toInt(), 8091);
    std::filesystem::remove_all(dir);
}

LOGOS_TEST(storage_config_honours_a_fixed_listen_port_so_a_peer_can_dial_us) {
    // The default listen-port is 0 (random). A node another agent must dial needs a known
    // address: fix the port and the address is /ip4/<extip>/tcp/<port>/p2p/<peerId>.
    std::string dir = scratchDir("listen");
    unsetenv("PILOT_STORAGE_LISTEN_PORT");
    LOGOS_ASSERT_FALSE(parseCfg(pilotStorageInitConfig(dir)).contains("listen-port"));
    setenv("PILOT_STORAGE_LISTEN_PORT", "8070", 1);
    QJsonObject cfg = parseCfg(pilotStorageInitConfig(dir));
    unsetenv("PILOT_STORAGE_LISTEN_PORT");
    LOGOS_ASSERT_EQ(cfg.value("listen-port").toInt(), 8070);
    std::filesystem::remove_all(dir);
}

LOGOS_TEST(storage_config_passes_bootstrap_nodes_as_a_json_array) {
    // libstorage's `bootstrap-node` is an ARRAY of SPRs. A comma list in the env var becomes
    // that array; unset means the key is absent and libstorage keeps its own default.
    std::string dir = scratchDir("boot");
    unsetenv("PILOT_STORAGE_BOOTSTRAP");
    LOGOS_ASSERT_FALSE(parseCfg(pilotStorageInitConfig(dir)).contains("bootstrap-node"));
    setenv("PILOT_STORAGE_BOOTSTRAP", "spr:AAA, spr:BBB", 1);
    QJsonObject cfg = parseCfg(pilotStorageInitConfig(dir));
    unsetenv("PILOT_STORAGE_BOOTSTRAP");
    QJsonArray boots = cfg.value("bootstrap-node").toArray();
    LOGOS_ASSERT_EQ(boots.size(), 2);
    LOGOS_ASSERT_EQ(boots[0].toString().toStdString(), std::string("spr:AAA"));
    LOGOS_ASSERT_EQ(boots[1].toString().toStdString(), std::string("spr:BBB"));
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
