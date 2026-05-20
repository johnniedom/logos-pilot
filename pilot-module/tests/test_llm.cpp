#include <logos_test.h>
#include "../src/pilot_llm.h"
#include "../src/pilot_impl.h"
#include <string>

LOGOS_TEST(noop_provider_not_configured) {
    NoOpProvider noop;
    LOGOS_ASSERT_FALSE(noop.isConfigured());
    LOGOS_ASSERT_EQ(noop.model(), std::string("none"));
    LOGOS_ASSERT_EQ(noop.providerName(), std::string("none"));
}

LOGOS_TEST(noop_provider_returns_empty) {
    NoOpProvider noop;
    std::string result = noop.complete("system", {{"user", "hello"}});
    LOGOS_ASSERT_EQ(result, std::string(""));
}

LOGOS_TEST(factory_returns_noop_when_no_env) {
    auto provider = createLLMProvider("", "");
    LOGOS_ASSERT(provider != nullptr);
    LOGOS_ASSERT_FALSE(provider->isConfigured());
    LOGOS_ASSERT_EQ(provider->providerName(), std::string("none"));
}

LOGOS_TEST(factory_returns_noop_for_unknown_provider) {
    auto provider = createLLMProvider("nonexistent", "");
    LOGOS_ASSERT(provider != nullptr);
    LOGOS_ASSERT_FALSE(provider->isConfigured());
}

LOGOS_TEST(process_owner_message_slash_command_bypasses_llm) {
    PilotImpl impl;
    std::string result = impl.processOwnerMessage("/balance");
    LOGOS_ASSERT_CONTAINS(result, "command");
    LOGOS_ASSERT_CONTAINS(result, "/balance");
}

LOGOS_TEST(process_owner_message_empty_returns_none) {
    PilotImpl impl;
    std::string result = impl.processOwnerMessage("");
    LOGOS_ASSERT_CONTAINS(result, "none");
}

LOGOS_TEST(process_owner_message_freetext_without_llm) {
    PilotImpl impl;
    std::string result = impl.processOwnerMessage("what's my balance?");
    LOGOS_ASSERT_CONTAINS(result, "command-only mode");
}

LOGOS_TEST(meta_status_shows_llm_none_by_default) {
    PilotImpl impl;
    std::string status = impl.metaStatus();
    LOGOS_ASSERT_CONTAINS(status, "\"llm\": \"none\"");
}
