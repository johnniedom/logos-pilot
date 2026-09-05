// pilot_owner — the OWNER side of a Pilot agent's owner channel, as a Logos module.
//
// Basecamp screens (QML) cannot sign or seal, so the Pilot Remote plugin (pilot-ui/remote-plugin)
// calls this module for everything that touches keys or the relay: logos.callModule("pilot_owner",
// "send", ["/balance"]) and so on. The work is done by OwnerClient (pilot-owner/src/owner_client.*),
// the same library the console client pilot-owner is built from; this class only turns its
// answers into JSON strings. The agent side (pilot-module) is unchanged and never contacted
// directly: everything goes through a Waku relay's REST API, sealed and signed.
//
// State: $PILOT_OWNER_HOME/state.json (default ~/.pilot-owner), mode 0600 — shared with the
// console client, so one owner key serves both front-ends.
//
// Every method returns JSON text. Failures are {"ok":false,"error":"<why>"}.
// Pure C++ types only in this header (the module builder generates the glue from it).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

class OwnerClient;

class PilotOwnerImpl {
public:
    PilotOwnerImpl();
    ~PilotOwnerImpl();

    // Load check: "echo: <text>".
    std::string echo(const std::string& text);

    // Make the owner keypair, or keep the one that exists. -> {ok, created, owner_pub, bind, state}
    // `bind` is the line the agent side needs (PILOT_OWNER_NPK=<pub> pilot deploy).
    std::string createKey();
    // Import "<priv hex>:<pub hex>" made elsewhere (e.g. by pilot-owner init). -> {ok, owner_pub}
    std::string importKey(const std::string& privColonPub);
    // Learn the agent: its Agent Card (JSON text, or a path to a file holding it), its account id
    // (metaStatus.account), the relay's REST URL ("" keeps the stored one, default
    // http://127.0.0.1:8645). -> {ok, agent_name, topic, relay}
    std::string pair(const std::string& cardJsonOrPath, const std::string& accountId,
                     const std::string& relayUrl);
    // Sign, seal and publish one line of text to the agent. -> {ok, nonce, sealed_bytes, topic}
    std::string send(const std::string& text);
    // New replies from the agent since the last poll (first poll: the last 15 minutes), oldest
    // first, each opened with the owner key. -> {ok, count, replies:[{text, time_ns, hash}]}
    std::string poll();
    // What this owner knows, never a secret: {initialised, paired, owner_pub, agent_name,
    // agent_key, account, topic, relay, last_nonce, state}
    std::string status();

private:
    std::unique_ptr<OwnerClient> client_;
    int64_t lastPollNs_;
};
