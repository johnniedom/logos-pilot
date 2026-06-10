#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
struct sqlite3;
class LogosAPI;
class LLMProvider;
class SkillRegistry;

class PilotImpl {
public:
    PilotImpl();
    ~PilotImpl();

    // Phase 0
    std::string echo(const std::string& input);

    // Phase 1: Identity + Wallet
    bool initialize(const std::string& dataDir);
    bool isInitialized();
    std::string getAgentNpk();
    std::string getAccountId();
    std::string walletBalance();
    std::string walletHistory();

    // Phase 2: Owner Channel
    bool establishOwnerChannel();
    bool sendToOwner(const std::string& message);
    std::string getOwnerChannelId();

    // Phase 3: Spending FSM
    std::string createSpendRequest(const std::string& recipient, int64_t amount, const std::string& reason);
    bool approveSpend(const std::string& requestId);
    bool rejectSpend(const std::string& requestId);
    int expireStaleSpends();   // auto-cancel pending requests past their deadline; returns count
    std::string getPendingSpends();
    bool setSpendingLimits(int64_t perTransaction, int64_t perPeriod, int64_t periodSeconds);
    std::string walletSend(const std::string& recipient, int64_t amount, const std::string& reason);

    // Phase 4: Storage skills
    std::string storageUpload(const std::string& path, const std::string& label);
    std::string storageDownload(const std::string& cid, const std::string& path);
    std::string storageList();
    std::string storageShare(const std::string& cid, const std::string& recipientNpk);

    // Phase 4: Messaging skills
    std::string messagingSend(const std::string& recipient, const std::string& message);
    bool messagingJoin(const std::string& groupId);
    std::string messagingCreateGroup(const std::string& membersJson);

    // Phase 4: Meta skills
    std::string metaSkills();
    std::string metaStatus();
    bool metaConfigure(const std::string& key, const std::string& value);

    // Phase 5: A2A + Agent skills
    std::string agentCard();
    std::string agentDiscover(const std::string& topic);
    std::string agentTask(const std::string& agentAddress, const std::string& skill, const std::string& paramsJson);
    std::string agentSubscribe(const std::string& agentAddress, const std::string& taskId);
    bool agentCancel(const std::string& agentAddress, const std::string& taskId);

    // Phase 5: Blockchain skills
    std::string programQuery(const std::string& programId, const std::string& paramsJson);
    std::string programCall(const std::string& programId, const std::string& instruction, const std::string& paramsJson);
    std::string programDeploy(const std::string& binaryPath);

    // LLM-assisted owner message processing
    std::string processOwnerMessage(const std::string& message);

    // Skill dispatch
    std::string dispatchSkill(const std::string& skillName, const std::string& argsJson);

    LogosAPI* logosAPI_ = nullptr;

private:
    void initDatabase(const std::string& dataDir);
    void initDependencyModules();
    void initStorageModule();
    void initDeliveryModule();
    bool initWallet();
    bool loadIdentity();
    bool createIdentity();
    bool fundAgentIfNeeded();
    void resetStaleIdentity();   // clear pilot.db identity + funded flag on wallet divergence
    void backupWallet();         // save wallet + keep a recovery copy of its storage file
    void recoverPendingTransactions();
    bool deliverToOwner(const std::string& payload);   // retrying, honest publish to owner channel
    void initLLM();
    std::string buildLLMSystemPrompt();
    sqlite3* db_ = nullptr;
    std::unique_ptr<LLMProvider> llm_;
    std::unique_ptr<SkillRegistry> registry_;
    std::string agentNpk_;
    std::string agentViewingKey_;
    std::string agentAccountId_;
    std::string ownerChannelId_;
    std::string ownerNpk_;
    std::string ownerName_;
    std::string agentEciesPub_;
    std::string agentEciesPriv_;
    std::string llmProvider_;
    std::string llmModel_;
    int64_t spendLimitPerTx_ = 100;
    int64_t spendLimitPerPeriod_ = 500;
    int64_t spendPeriodSeconds_ = 86400;
    std::string dataDir_;
    bool initialized_ = false;
    bool walletOpened_ = false;
    bool storageInitialized_ = false;
    bool deliveryInitialized_ = false;
    bool depsInitialized_ = false;
    std::vector<std::pair<std::string,std::string>> chatHistory_;
};
