#include <logos_test.h>
#include "../src/pilot_impl.h"
#include <string>
#include <cstring>

LOGOS_TEST(echo_returns_input) {
    PilotImpl impl;
    LOGOS_ASSERT_EQ(impl.echo("hello"), std::string("echo: hello"));
}

LOGOS_TEST(echo_handles_empty_input) {
    PilotImpl impl;
    LOGOS_ASSERT_EQ(impl.echo(""), std::string("echo: "));
}

LOGOS_TEST(not_initialized_by_default) {
    PilotImpl impl;
    LOGOS_ASSERT_FALSE(impl.isInitialized());
}

LOGOS_TEST(npk_empty_before_init) {
    PilotImpl impl;
    LOGOS_ASSERT_EQ(impl.getAgentNpk(), std::string(""));
}

LOGOS_TEST(account_id_empty_before_init) {
    PilotImpl impl;
    LOGOS_ASSERT_EQ(impl.getAccountId(), std::string(""));
}

LOGOS_TEST(owner_channel_empty_before_init) {
    PilotImpl impl;
    LOGOS_ASSERT_EQ(impl.getOwnerChannelId(), std::string(""));
}

LOGOS_TEST(wallet_balance_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.walletBalance();
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(wallet_history_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.walletHistory();
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(wallet_send_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.walletSend("recipient", 100, "test");
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(storage_list_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.storageList();
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(storage_upload_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.storageUpload("/tmp/test.txt", "test");
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(messaging_send_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.messagingSend("recipient", "hello");
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(agent_card_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.agentCard();
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(agent_discover_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.agentDiscover("");
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(meta_skills_lists_all_skills) {
    PilotImpl impl;
    std::string result = impl.metaSkills();
    LOGOS_ASSERT_CONTAINS(result, "wallet.balance");
    LOGOS_ASSERT_CONTAINS(result, "wallet.send");
    LOGOS_ASSERT_CONTAINS(result, "storage.upload");
    LOGOS_ASSERT_CONTAINS(result, "storage.download");
    LOGOS_ASSERT_CONTAINS(result, "messaging.send");
    LOGOS_ASSERT_CONTAINS(result, "agent.card");
    LOGOS_ASSERT_CONTAINS(result, "agent.ask");
    LOGOS_ASSERT_CONTAINS(result, "agent.discover");
    LOGOS_ASSERT_CONTAINS(result, "program.query");
    LOGOS_ASSERT_CONTAINS(result, "meta.skills");
    LOGOS_ASSERT_CONTAINS(result, "meta.status");
    LOGOS_ASSERT_CONTAINS(result, "\"count\":22");
}

LOGOS_TEST(meta_status_shows_not_initialized) {
    PilotImpl impl;
    std::string result = impl.metaStatus();
    LOGOS_ASSERT_CONTAINS(result, "\"initialized\":false");
}

LOGOS_TEST(establish_owner_channel_fails_without_api) {
    PilotImpl impl;
    LOGOS_ASSERT_FALSE(impl.establishOwnerChannel());
}

LOGOS_TEST(send_to_owner_fails_without_channel) {
    PilotImpl impl;
    LOGOS_ASSERT_FALSE(impl.sendToOwner("test message"));
}

LOGOS_TEST(messaging_join_fails_without_api) {
    PilotImpl impl;
    LOGOS_ASSERT_FALSE(impl.messagingJoin("group123"));
}

LOGOS_TEST(agent_cancel_fails_without_api) {
    PilotImpl impl;
    LOGOS_ASSERT_FALSE(impl.agentCancel("addr", "task123"));
}
