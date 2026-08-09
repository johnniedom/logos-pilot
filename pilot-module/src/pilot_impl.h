#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "logos_module_context.h"
struct sqlite3;
class LLMProvider;
class SkillRegistry;

// Single source of truth for the A2A asker-pays-doer service set (FIX 4a + FIX 2: SAFE-only).
// Each entry is a SAFE skill we AUTO-SERVICE for a stranger on the inbound leg (we run the
// real skill and get PAID for it) paired with the LEZ price we advertise for it. BOTH
// agentCard()'s _logos.pricing AND processInboundRequest()'s inbound auto-dispatch set are
// derived from this one table, so the advertised-as-autonomous price set can never diverge
// from the auto-serviced-skill set.
//
// SAFE means pure compute with NO side effects on this agent: no local-file access, no use of
// the agent's messaging identity, no movement of funds. Such skills can run for an UNKNOWN
// peer with no owner involvement. agent-ask (LLM Q&A) is the canonical SAFE paid service.
//
// RISKY skills (storage-*, messaging-*, wallet-send, program-*) touch local files, the
// agent's messaging identity, or funds, so they MUST NOT appear here: an A2A peer requesting
// one is routed to the OWNER GATE (input-required) by processInboundRequest, never auto-run
// (a2aRiskyOwnerGated). They are advertised on the Agent Card as owner-gated, not priced.
// program-call/program-deploy are additionally unsupported over A2A.
//
// A price of 0 means "serviced but explicitly free by intent" and is NOT advertised in
// pricing. Pure C++ (no Qt) so it can live in the universal header. External linkage so tests
// assert on it.
struct A2AService {
    const char* id;    // A2A skill id, dash form (e.g. "agent-ask")
    int64_t price;     // advertised LEZ price; 0 == serviced but explicitly free
};
const std::vector<A2AService>& a2aServiceCatalog();

// Derives LogosModuleContext (2026-08 SDK): the generated glue no longer hands the impl a
// LogosAPI — dependencies are reached ONLY through the typed clients in the generated
// LogosModules umbrella, via modules().lez_core / .delivery_module / .storage_module
// (logos_sdk.h, generated per-build from metadata.json#dependencies; verified from the
// kept build sandbox 2026-08-09 — the umbrella carries NO raw API pointer, so there is no
// bridge shortcut). isContextReady() is the "framework attached" guard replacing the old
// `if (!logosAPI_)` checks: impls constructed directly (unit tests) have it false and every
// dependency-touching path must skip, exactly like the old null-API behavior. modules() is
// NOT valid inside onContextReady() — the glue sets the context before the modules pointer.
class PilotImpl : public LogosModuleContext {
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
    // Quietly move a HELD/NOTIFIED spend to REJECTED WITHOUT notifying the owner or resuming a
    // linked task (L3/P7): used when a peer withdraws its task (tasks/cancel) so a later owner
    // /approve can no longer move funds for a canceled task. Unlike rejectSpend this emits no
    // "Transaction ... rejected." owner message (which would be misleading peer-driven spam).
    bool releaseHeldSpend(const std::string& requestId);
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
    // Learn a peer from a card handed over out-of-band (file, paste, QR) instead of waiting
    // for one to arrive over discovery. Runs the SAME signature + TOFU verification as a
    // broadcast card, so an imported peer is neither more nor less trusted than a discovered
    // one — only the delivery differs.
    std::string agentImportCard(const std::string& cardJson);

    // A card that arrived on the shared discovery topic. Runs the SAME verification and TOFU
    // pinning as an imported card — only the delivery differs. Junk is ignored quietly: this
    // is a public channel and anyone can publish to it.
    void handleDiscoveryCard(const std::string& cardJson);

    // ---- Open for hire (the OWNER's decision, never the agent's) --------------------------
    // An agent does NOT put itself up for sale on boot. agentOpenForHire() is the moment the
    // owner says "take work from strangers": it subscribes the agent's own inbox(es) and lets
    // agentCard() broadcast. agentCloseForHire() takes it back off the market.
    //
    // REMEMBERED across restarts (config "a2a.open_for_hire") so the owner says it ONCE. A
    // per-boot decision would mean every restart, crash or redeploy silently dropped the agent
    // off the market with nothing to say so — the same class of failure as the bug that made
    // this split necessary, only with a different trigger.
    //
    // Closed means STRANGERS CANNOT HIRE US, not that we go deaf: the shared discovery channel
    // stays subscribed either way, so a closed agent keeps learning who else is out there and
    // can still hire THEM. The buyer side works while the seller side is shut.
    bool agentOpenForHire();
    bool agentCloseForHire();
    bool agentIsOpenForHire();

    // Every topic this identity listens on RIGHT NOW: the shared discovery channel always, and
    // the agent's own inbox(es) — the enc-key inbox the card advertises as _logos.enc_key, plus
    // the legacy signing-key inbox when it differs — only while open for hire. PURE (no I/O) so
    // the advertise/listen invariant is testable without a delivery module. Empty keys
    // contribute no inbox entry: an agent with no identity must never claim to listen on
    // "/pilot/1/inbox-/proto".
    std::vector<std::string> identityTopics();

    // Ask delivery_module to subscribe every identityTopics() entry not already asked for, and
    // drop any subscription no longer in that set (which is how closing for hire actually stops
    // strangers reaching us). Idempotent, so it is safe to call on every path that could be the
    // first to run after an identity appears. This CANNOT live in initDeliveryModule(): that
    // runs BEFORE the identity is loaded and only once, so it found empty keys, skipped both
    // subscribes and never retried — which is why an agent advertised an address it was not
    // listening on (measured 2026-07-26).
    void subscribeIdentityTopics();

    // Where the agent is ACTUALLY listening — every topic delivery_module confirmed, not every
    // topic we wanted. identityTopics() is intent; this is the result, and the two differing is
    // exactly the condition that made an agent unhireable.
    //
    // This exists because there is no other way to observe it: delivery_module logs a `send`
    // with its content topic but logs NOTHING for a `subscribe` (measured 2026-07-27 by
    // subscribing a canary topic directly — it appears nowhere in a 600KB log). So a
    // log-scraping assertion can never confirm a subscription, however correct the code is.
    // Ask the agent instead.
    std::vector<std::string> subscribedTopics();

    std::string agentTask(const std::string& agentAddress, const std::string& skill, const std::string& paramsJson);
    std::string agentSubscribe(const std::string& agentAddress, const std::string& taskId);
    bool agentCancel(const std::string& agentAddress, const std::string& taskId);

    // SAFE paid A2A service (FIX 2): answer a prompt with the agent's LLM. PURE COMPUTE —
    // it never touches local files, the agent's messaging identity, or funds — so it is safe
    // to auto-run for an UNKNOWN peer (and is the skill the autonomous-pay demo exercises).
    // Uses a self-contained system prompt that exposes NO owner context and NO tool dispatch.
    // Returns {"answer": ...} on success; an honest {"error": ...} when no LLM is configured
    // or the provider errors (we NEVER fabricate an answer). Public so the builtin skill and
    // tests call it directly.
    std::string agentAsk(const std::string& prompt);

    // Inbound A2A task server: handle a decrypted JSON-RPC request from a peer agent,
    // returning the JSON-RPC reply. Public so tests drive the state machine directly.
    // authenticatedNpk is the transport-authenticated sender (from verifyInboundRequest, H2);
    // empty for direct FSM unit calls, in which case ownership binding is skipped and the owner
    // prompt renders the sender UNVERIFIED. tasks/cancel and tasks/sendSubscribe require it to
    // equal the task's stored sender_npk. senderFirstContact is true only on the first
    // authenticated contact for this npk (feeds the owner prompt's trust label, L4).
    // NOTE: no default arguments. The universal-module generator parses this header and rejects
    // a defaulted parameter ("parameter 'false' is `bool senderFirstContact =`, which has no
    // LIDL type"), so every caller passes all three explicitly — "" and false are what the old
    // defaults meant.
    std::string processInboundRequest(const std::string& requestJson,
                                      const std::string& authenticatedNpk,
                                      bool senderFirstContact);

    // Inbound A2A transport entry: decrypt a raw ECIES payload from our inbox topic with
    // agentEciesPriv_ — the SAME ECIES key our Agent Card advertises as BOTH the inbox id
    // and _logos.signing_key. A requester encrypts the task to that key (the one we HOLD
    // the private half of), NOT the wallet viewing key (whose private half lives in
    // wallet-ffi, so we could never decrypt a task sent to it). Then run it through
    // processInboundRequest and reply to the peer. Public so tests drive the full
    // decrypt+dispatch round trip, not just the pure state machine in isolation.
    void handleInboundA2A(const std::string& encryptedPayload);

    // Requester-side settlement of a peer server's A2A reply for an outbound task WE
    // submitted. Pure FSM logic (no transport, no ECIES) so tests drive it directly;
    // handleA2AReply is the ECIES wrapper that decrypts a reply and calls this.
    // ASKER PAYS THE DOER: settle ONLY on the doer's terminal success ('completed') —
    // pay the doer's DECLARED, AUTHENTICATED (verifyCardStatus=='valid') payout the card
    // price through the spending FSM. Progress (accepted/working/input-required) is
    // non-settling and leaves the row 'submitted'; NEVER pay on failed/canceled/rejected.
    // Settles AT MOST ONCE via the atomic submitted->settling claim.
    void settleOutboundReply(const std::string& taskId, const std::string& state);

    // Signature gate for a DECRYPTED outbound reply (FIX 1 — the BLOCKER), then settle.
    // A reply is only ECIES-ENCRYPTED to OUR public key, and a public key + the reply topic
    // are PUBLIC, so ANY observer can encrypt a forged {"status":{"state":"completed"}} and
    // force us (the asker) to pay — encryption to a public key authenticates NOTHING. So
    // before settling we require the reply to be SIGNED by the doer's AUTHORITATIVE
    // signing_key (the key bound to this task's agent_address by its identity-validated,
    // TOFU-pinned Agent Card), verifying the doer's signature over the canonical reply bytes
    // against THAT key — never the reply-supplied key. An unsigned reply, a reply signed by a
    // non-pinned/mismatched key, or a reply for which we hold no authoritative card is DROPPED
    // (ambiguity -> inaction, NO settle, NO pay). Public so tests drive the gate without ECIES;
    // handleA2AReply decrypts a peer's reply then calls this.
    void verifyAndSettleReply(const std::string& taskId, const std::string& replyJson);

    // Run the on-chain transfer for a spend request that already cleared its gate
    // (owner-approved or auto-approved below threshold): EXECUTING -> private transfer ->
    // COMPLETED/TX_FAILED. Returns true iff the transfer succeeded. Idempotent: a no-op on
    // COMPLETED/EXECUTING/TX_FAILED/REJECTED/EXPIRED so a duplicate trigger never re-runs a
    // transfer. Public so tests can drive the idempotency guard directly. Pure execution:
    // it does NOT notify the owner or drive any linked task — callers own that.
    bool executeSpend(const std::string& requestId);

    // L7 — startup crash reconciliation: every spend stranded in EXECUTING by a crash
    // (a clean run always drives EXECUTING->terminal synchronously) is moved to the new
    // terminal TX_UNKNOWN, its linked outbound ('settling'->'pay-unresolved') and inbound
    // ('input-required'->failed) tasks are un-hung, and the owner is surfaced ONCE. The wallet
    // exposes no status-by-hash query, so we conservatively mark unknown (still budget-counted)
    // rather than guess COMPLETED/TX_FAILED. No-op without db_. Public so tests drive it directly
    // and so pilot_identity.cpp can call it on the startup driver (see §3.0).
    void reconcileExecutingSpends();

    // Phase 5: Blockchain skills
    std::string programQuery(const std::string& programId, const std::string& paramsJson);
    std::string programCall(const std::string& programId, const std::string& instruction, const std::string& paramsJson);
    std::string programDeploy(const std::string& binaryPath);

    // LLM-assisted owner message processing
    std::string processOwnerMessage(const std::string& message);

    // M1 — owner-channel authentication, FAIL-OPEN. Given the DECRYPTED owner-channel payload
    // `raw`, decide whether to process it and yield the inner owner message in `innerOut`.
    //
    // FAIL-OPEN by design: the current owner clients (pilot-cli, pilot-ui) send RAW TEXT and do
    // NOT sign, so a raw-text payload (or any non-envelope) is ACCEPTED unchanged (innerOut=raw,
    // returns true). This NEVER locks the owner out of their own agent. It only HARDENS the path
    // for a client that opts in to signing: when `raw` parses as a JSON object carrying a
    // `message` string AND an `_logos` object with BOTH `signing_key` and `signature`, it is
    // treated as a SIGNED ENVELOPE and must pass (a) an ECDSA signature over the canonical bytes
    // (envelope minus `_logos.signature`, Compact — the SAME scheme as signA2AEnvelope /
    // verifyInboundRequest), (b) a TOFU pin of the owner signing key (config "owner.signing_key"),
    // and (c) a strictly-monotonic replay nonce (config "owner.last_nonce"); ANY failure returns
    // false and the caller drops the message. On success innerOut is the inner `message`.
    //
    // FOLLOW-UP (fail-closed): once pilot-cli/pilot-ui SIGN every owner message, the fail-open
    // ELSE branch can be flipped to `return false` to REJECT unsigned owner traffic. That is a
    // documented client-coordinated change — do NOT make it fail-closed here, or the existing
    // unsigned clients would be locked out. PRIVATE (declared below) so the QRO generator never
    // wraps it. Its logic mirrors verifyInboundRequest (which IS unit-tested); a direct test seam
    // is intentionally avoided because the generator wraps any pilot_impl.h decl taking a PilotImpl&.

    // Dependency-injection seam for the LLM provider — a FRIEND FREE FUNCTION (declared after
    // this class), NOT a public member: the universal-module code generator wraps every public
    // method for Qt Remote Objects and cannot marshal a std::unique_ptr<LLMProvider>. Tests call
    // pilotSetLLMProvider(impl, ...) to drive agent.ask deterministically; deploy-time LLM
    // selection goes through metaConfigure (llm.provider/llm.api_key) + initLLM(), not RPC.
    friend void pilotSetLLMProvider(PilotImpl& impl, std::unique_ptr<LLMProvider> provider);

    // Skill dispatch
    std::string dispatchSkill(const std::string& skillName, const std::string& argsJson);

private:
    // M1 owner-channel auth (see the comment in the public section + a2a-deferred-handoff.md).
    // PRIVATE so the QRO generator never wraps it (its std::string& out-param can't be marshalled).
    // Wired into the owner branch in initDeliveryModule; its logic mirrors the unit-tested
    // verifyInboundRequest, and M1's wire path is validated by the manual owner-channel test.
    bool verifyOwnerMessage(const std::string& raw, std::string& innerOut);
    void initDatabase(const std::string& dataDir);
    void initStorageModule();
    void initDeliveryModule();
    bool initWallet();
    bool loadIdentity();
    bool createIdentity();
    // M2: write a secret hex value (the agent's ecies.priv) into config, AES-256-GCM
    // wrapped at rest when PILOT_KEY_PASSPHRASE is set; otherwise stored plaintext with a
    // warning. configKey is only ever a fixed literal (never peer input).
    bool persistSecretConfig(const std::string& configKey, const std::string& clearHex);
    // Write the owner's open/closed-for-hire decision to config so it survives a restart.
    bool persistOpenForHire(bool open);
    // L1: decrypt an inbound ECIES payload trying the dedicated encryption key (agentEncPriv_)
    // first, then the legacy signing key (agentEciesPriv_) for pre-split peers that still
    // encrypt to _logos.signing_key. Returns true (out = plaintext) on the first key that
    // decrypts; false (out untouched) when neither key works or both are empty.
    bool a2aTryDecrypt(const std::string& payload, std::string& out) const;
    bool fundAgentIfNeeded();
    void resetStaleIdentity();   // clear pilot.db identity + funded flag on wallet divergence
    void backupWallet();         // save wallet + keep a recovery copy of its storage file
    void recoverPendingTransactions();
    // Move a freshly-created spend request into the owner-approval flow: HELD -> notify ->
    // NOTIFIED only if the owner notification actually delivered (else keep it HELD so we
    // never claim an approval prompt that failed, and it is re-announced on recovery).
    // Shared by the per-transaction and per-period limit branches of walletSend.
    std::string holdForApproval(const std::string& reqId, const std::string& ownerMsg,
                                const std::string& heldMessage);
    // Sum of amounts committed within the current rolling period (COMPLETED/EXECUTING/
    // APPROVED). Shared by walletSend and the inbound A2A auto-approve gate so both use
    // identical per-period accounting (impl in pilot_spending.cpp).
    int64_t periodSpent();
    bool deliverToOwner(const std::string& payload);   // retrying, honest publish to owner channel
    // Inbound A2A internals (impl in pilot_a2a_inbox.cpp). handleInboundA2A is the public
    // transport entry (declared above so tests can drive the decrypt+dispatch round trip).
    // I2: build + sign the Agent Card and RETURN it with NO network side effect. agentCard()
    // (public discovery skill) calls this then publishes; the inbound 'capabilities' read calls
    // this and replies WITHOUT a discovery-topic write. Private so the QRO generator never wraps
    // it as a remote method; member callers in other TUs still reach it.
    std::string buildCard();
    bool replyToPeer(const std::string& topic, const std::string& recipientKey, const std::string& json);
    void inboundTaskSetState(const std::string& taskId, const std::string& state, const std::string& resultJson);
    void inboundTasksRecover();
    // Outbound recovery on restart (M7): re-subscribe reply topics for still-open
    // ('submitted') outbound tasks so a peer's accept/complete reply can still settle, and
    // reconcile tasks caught mid-settle ('settling') against their linked spend_request
    // (COMPLETED -> 'paid', terminal-failed -> 'pay-failed', else left for retry) so a
    // crash mid-settle never orphans. Impl in pilot_a2a.cpp.
    void outboundTasksRecover();
    void resumeInboundTask(const std::string& spendRequestId, bool approved, const std::string& detail);
    // Outbound A2A (requester side, impl in pilot_a2a.cpp): the ECIES/transport wrapper
    // that consumes a peer server's reply on "/pilot/1/reply-<id>/proto", decrypts it
    // with agentEciesPriv_ (the doer encrypted to our sender_ecies per the A2A contract),
    // and hands the (taskId, state) to settleOutboundReply for the pay-on-acceptance FSM.
    void handleA2AReply(const std::string& topic, const std::string& payload);
    // Declared price for a skill from a discovered Agent Card (_logos.pricing), matched
    // on ONE canonical identity field (the encryption/routing key agentTask uses); 0 when
    // no card or no price is known (we never fabricate a price).
    int64_t discoveredPriceFor(const std::string& agentAddress, const std::string& skill);
    // Payout account (_logos.payout) for a discovered Agent Card matched on that SAME
    // canonical identity. Empty when no card/payout is on file — the requester then marks
    // the outbound task 'pay-failed' and pays NOTHING (never the messaging address, M5).
    std::string discoveredPayoutFor(const std::string& agentAddress);
    // ECIES routing/encryption key for an OUTBOUND A2A message to `agentAddress`. A2A
    // messaging is unified on the ECIES key — the doer HOLDS that private key, subscribes
    // its inbox under it, and publishes it as _logos.signing_key — NOT the wallet viewing
    // key (whose private half lives in wallet-ffi, so the doer could never decrypt). Resolves
    // the doer's published _logos.signing_key from its discovered Agent Card; with no card on
    // file, treats agentAddress itself as the key (the caller passed the ECIES key directly)
    // — NEVER extractEncryptionKey(npk), which would yield a key the doer cannot read.
    std::string a2aRoutingKeyFor(const std::string& agentAddress);
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
    // L1 key separation: an INDEPENDENT ECDH/ECIES keypair, distinct from agentEcies* (which
    // stays the ECDSA SIGNING identity == _logos.signing_key, TOFU-pinned by every peer). Peers
    // encrypt inbound A2A tasks+replies to this key (advertised as _logos.enc_key); we decrypt
    // with agentEncPriv_. The signing key is never used for ECDH on new traffic; legacy peers
    // that still encrypt to the signing key are handled by a2aTryDecrypt's ecies fallback.
    std::string agentEncPub_;
    std::string agentEncPriv_;
    // The encryption/routing key WE advertise and subscribe under. Falls back to the signing
    // ECIES key for a pre-split DB that has no enc keypair yet (back-compat).
    std::string a2aSelfEncKey() const { return agentEncPub_.empty() ? agentEciesPub_ : agentEncPub_; }
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
    // The owner's standing decision to take work from strangers, restored from config
    // "a2a.open_for_hire" by loadIdentity(). Closed until said otherwise.
    bool openForHire_ = false;
    // Topics we have actually ASKED delivery_module for, so a repeat subscribe is a no-op and
    // a topic that leaves identityTopics() (closing for hire) can be unsubscribed exactly once.
    std::vector<std::string> subscribedTopics_;
    // L6 — count of LLM complete() calls currently blocking this single delivery thread. Plain
    // int (not atomic): guards SYNCHRONOUS nested-QEventLoop re-entrancy, not OS threads.
    int llmInFlight_ = 0;
    std::vector<std::pair<std::string,std::string>> chatHistory_;
};

// Test/DI seam for the LLM provider: installs `provider` (or NoOpProvider when null) into impl.
// A free function (not a PilotImpl method) so the Qt Remote Objects generator never tries to
// marshal std::unique_ptr<LLMProvider>; friended above for access to the private llm_ member.
void pilotSetLLMProvider(PilotImpl& impl, std::unique_ptr<LLMProvider> provider);
