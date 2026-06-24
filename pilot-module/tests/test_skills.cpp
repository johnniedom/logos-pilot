#include <logos_test.h>
#include "../src/pilot_skill.h"
#include "../src/pilot_impl.h"
#include <string>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <unistd.h>

LOGOS_TEST(registry_starts_empty) {
    SkillRegistry reg;
    LOGOS_ASSERT_EQ(reg.count(), size_t(0));
}

LOGOS_TEST(registry_register_and_count) {
    SkillRegistry reg;
    reg.registerSkill(std::make_unique<LambdaSkill>(
        "test.skill", "test", "A test skill", "{}", "{}", 0,
        [](const std::string&) { return "{\"ok\": true}"; }
    ));
    LOGOS_ASSERT_EQ(reg.count(), size_t(1));
    LOGOS_ASSERT_TRUE(reg.hasSkill("test.skill"));
    LOGOS_ASSERT_FALSE(reg.hasSkill("nonexistent"));
}

LOGOS_TEST(registry_dispatch_executes_skill) {
    SkillRegistry reg;
    reg.registerSkill(std::make_unique<LambdaSkill>(
        "echo.test", "test", "Echo skill", "{}", "{}", 0,
        [](const std::string& args) { return "{\"input\": \"" + args + "\"}"; }
    ));

    std::string result = reg.dispatch("echo.test", "hello");
    LOGOS_ASSERT_CONTAINS(result, "hello");
}

LOGOS_TEST(registry_dispatch_unknown_skill_returns_error) {
    SkillRegistry reg;
    std::string result = reg.dispatch("no.such.skill", "{}");
    LOGOS_ASSERT_CONTAINS(result, "error");
    LOGOS_ASSERT_CONTAINS(result, "unknown skill");
}

LOGOS_TEST(registry_list_skills_returns_json) {
    SkillRegistry reg;
    reg.registerSkill(std::make_unique<LambdaSkill>(
        "wallet.balance", "wallet", "Check balance", "{}", "{}", 0,
        [](const std::string&) { return "{}"; }
    ));
    std::string list = reg.listSkills();
    LOGOS_ASSERT_CONTAINS(list, "wallet.balance");
    LOGOS_ASSERT_CONTAINS(list, "\"count\":1");
}

LOGOS_TEST(registry_list_skills_for_card_format) {
    SkillRegistry reg;
    reg.registerSkill(std::make_unique<LambdaSkill>(
        "storage.upload", "storage", "Upload a file", "{}", "{}", 10,
        [](const std::string&) { return "{}"; }
    ));
    std::string card = reg.listSkillsForCard();
    LOGOS_ASSERT_CONTAINS(card, "storage-upload");
    LOGOS_ASSERT_CONTAINS(card, "application/json");
}

LOGOS_TEST(builtin_skills_registers_22) {
    PilotImpl impl;
    std::string skills = impl.metaSkills();
    LOGOS_ASSERT_CONTAINS(skills, "\"count\":22");   // +1: SAFE A2A service agent.ask (FIX 2)
    LOGOS_ASSERT_CONTAINS(skills, "agent.ask");
}

LOGOS_TEST(dispatch_skill_works_on_echo_equivalent) {
    PilotImpl impl;
    std::string result = impl.dispatchSkill("wallet.balance", "{}");
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

// --- Runtime plugin loader (Usability #1) ---------------------------------------
// These exercise the TRUST GATE and the load-failure isolation of
// SkillRegistry::loadPlugins WITHOUT needing a real compiled .so. Building a valid
// plugin would require a separate Qt shared library; the contract we can verify here
// is the safety-critical one: default-off, and a malformed library is skipped (never
// crashes, never registers).

// Default-off: with PILOT_ENABLE_PLUGINS unset, loadPlugins is completely inert even
// when pointed at a path — no scan, no change to the registry.
LOGOS_TEST(load_plugins_disabled_by_default_is_noop) {
    unsetenv("PILOT_ENABLE_PLUGINS");
    SkillRegistry reg;
    reg.registerSkill(std::make_unique<LambdaSkill>(
        "builtin.skill", "test", "builtin", "{}", "{}", 0,
        [](const std::string&) { return "{}"; }));
    size_t before = reg.count();
    reg.loadPlugins(".");                       // would-be scan target; must be ignored
    LOGOS_ASSERT_EQ(reg.count(), before);       // unchanged
    LOGOS_ASSERT_TRUE(reg.hasSkill("builtin.skill"));
}

// Enabled but the directory does not exist: safe no-op, no crash.
LOGOS_TEST(load_plugins_enabled_missing_dir_is_safe) {
    setenv("PILOT_ENABLE_PLUGINS", "1", 1);
    SkillRegistry reg;
    reg.loadPlugins("/pilot/definitely/no/such/plugins/dir");
    LOGOS_ASSERT_EQ(reg.count(), size_t(0));
    unsetenv("PILOT_ENABLE_PLUGINS");
}

// An explicit "off" value (e.g. "0") must NOT enable scanning.
LOGOS_TEST(load_plugins_explicit_zero_stays_disabled) {
    setenv("PILOT_ENABLE_PLUGINS", "0", 1);
    SkillRegistry reg;
    reg.loadPlugins(".");
    LOGOS_ASSERT_EQ(reg.count(), size_t(0));
    unsetenv("PILOT_ENABLE_PLUGINS");
}

// Load-failure isolation: a file with a shared-library extension that is NOT a valid
// plugin must be logged + SKIPPED. The module survives and nothing is registered.
LOGOS_TEST(load_plugins_skips_invalid_library) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() /
        ("pilot_plugin_test_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    {
        std::ofstream f((dir / "bogus.so").string(), std::ios::binary);
        f << "this is not a valid ELF shared object";
    }
    // A non-library file in the same dir must simply be ignored (not even attempted).
    {
        std::ofstream f((dir / "README.txt").string());
        f << "not a plugin";
    }

    setenv("PILOT_ENABLE_PLUGINS", "1", 1);
    SkillRegistry reg;
    reg.loadPlugins(dir.string());              // must not crash on the bogus .so
    LOGOS_ASSERT_EQ(reg.count(), size_t(0));    // bogus library registered nothing
    unsetenv("PILOT_ENABLE_PLUGINS");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// --- Positive path against a REAL compiled plugin -------------------------------
// PILOT_TEST_PLUGIN_DIR is defined by tests/CMakeLists.txt when it can build the
// bundled weather example (examples/skill-weather) into an actual loadable Qt
// plugin .so. These tests prove the Usability #1 guarantee end to end: with
// PILOT_ENABLE_PLUGINS set, a genuine third-party .so is discovered, its skill
// REGISTERS, and it DISPATCHES — and the core module was NOT recompiled to add it
// (the weather skill is not a builtin). They are skipped only where Qt6 wasn't
// directly findable at configure time (the safety-critical negative-path tests
// above always run).
#ifdef PILOT_TEST_PLUGIN_DIR
LOGOS_TEST(load_plugins_registers_and_dispatches_real_plugin) {
    setenv("PILOT_ENABLE_PLUGINS", "1", 1);
    SkillRegistry reg;
    // Precondition: weather.lookup is NOT compiled into the core registry.
    LOGOS_ASSERT_FALSE(reg.hasSkill("weather.lookup"));

    reg.loadPlugins(PILOT_TEST_PLUGIN_DIR);     // scan the dir holding the real .so

    // The plugin's skill is now registered purely from the loaded shared object.
    LOGOS_ASSERT_TRUE(reg.hasSkill("weather.lookup"));
    LOGOS_ASSERT_EQ(reg.count(), size_t(1));

    // And it actually dispatches: the plugin's native code runs and echoes input.
    std::string out = reg.dispatch("weather.lookup", "{\"location\": \"Berlin\"}");
    LOGOS_ASSERT_CONTAINS(out, "Berlin");
    LOGOS_ASSERT_CONTAINS(out, "temp_c");
    unsetenv("PILOT_ENABLE_PLUGINS");
}

// A real plugin must NOT be able to shadow an existing (e.g. builtin) skill: on a
// name clash the incumbent is kept and the plugin's skill is skipped — verified
// here against the actual loaded .so, not a stub.
LOGOS_TEST(load_plugins_real_plugin_does_not_shadow_existing_skill) {
    setenv("PILOT_ENABLE_PLUGINS", "1", 1);
    SkillRegistry reg;
    reg.registerSkill(std::make_unique<LambdaSkill>(
        "weather.lookup", "builtin", "incumbent", "{}", "{}", 0,
        [](const std::string&) { return "{\"from\":\"incumbent\"}"; }));

    reg.loadPlugins(PILOT_TEST_PLUGIN_DIR);     // same name -> plugin skill skipped

    LOGOS_ASSERT_EQ(reg.count(), size_t(1));    // not double-registered
    std::string out = reg.dispatch("weather.lookup", "{}");
    LOGOS_ASSERT_CONTAINS(out, "incumbent");    // incumbent kept, plugin did not win
    unsetenv("PILOT_ENABLE_PLUGINS");
}
#endif  // PILOT_TEST_PLUGIN_DIR
