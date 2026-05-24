#include <logos_test.h>
#include "../src/pilot_skill.h"
#include "../src/pilot_impl.h"
#include <string>

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

LOGOS_TEST(builtin_skills_registers_21) {
    PilotImpl impl;
    std::string skills = impl.metaSkills();
    LOGOS_ASSERT_CONTAINS(skills, "\"count\":21");
}

LOGOS_TEST(dispatch_skill_works_on_echo_equivalent) {
    PilotImpl impl;
    std::string result = impl.dispatchSkill("wallet.balance", "{}");
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}
