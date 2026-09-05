// owner_client.h — the owner's half of a Pilot agent's owner channel, as a library.
//
// Two front-ends compile this one file in and therefore put the SAME bytes on the wire:
//   pilot-owner  (pilot-owner/src/main.cpp)             — the console client, proven on the public testnet
//   pilot_owner  (pilot-owner/module/src/pilot_owner_impl.cpp) — the module the Basecamp plugin talks to
// The agent side (pilot-module) is untouched; what the agent expects is spelled out in
// docs/owner-channel.md and reproduced by buildEnvelope / verifyEnvelope below.
//
// The channel: one Logos Messaging content topic, "/pilot/1/owner-<agent account id>/proto".
// Owner -> agent: a signed envelope {"message": <text>, "_logos": {"signing_key": <owner pub>,
// "nonce": <strictly increasing int>, "signature": <ECDSA hex over the compact JSON of the
// envelope with the signature absent>}}, ECIES-sealed to the agent's signing key (the card's
// _logos.signing_key). Agent -> owner: plain text, ECIES-sealed to the owner's key, same topic.
// Transport: a Waku relay's REST API only — publish = POST /relay/v1/auto/messages, read =
// GET /store/v3/messages. No daemon socket, no local RPC, no server of ours anywhere between.
//
// State (keys, pairing, nonce high-water, seen message hashes) is one JSON file, mode 0600, in
// the directory given to the constructor ($PILOT_OWNER_HOME, default ~/.pilot-owner). Every
// operation reads it and writes it back, so a console client and a module can share one owner.
//
// This header is Qt-free on purpose; the implementation uses Qt JSON (the agent's canonical
// bytes are Qt's compact JSON) and Qt Network. Only pilot_crypto.h is needed to include it.
#pragma once

#include "pilot_crypto.h"

#include <cstdint>
#include <string>
#include <vector>

struct OwnerReply {
    std::string text;        // the agent's reply, decrypted
    int64_t timestampNs = 0; // the relay's message timestamp (0 when the relay gave none)
    std::string hash;        // the relay's messageHash — the de-duplication key
};

class OwnerClient {
public:
    // stateDir empty = $PILOT_OWNER_HOME, else ~/.pilot-owner.
    explicit OwnerClient(std::string stateDir = "");
    static std::string defaultStateDir();
    std::string stateDir() const { return dir_; }
    std::string statePath() const;

    // Make the owner keypair. An existing key is kept (createdOut = false) unless force is set —
    // the agent bound to the old key would stop listening to you. ownerPubOut = the public key.
    bool init(bool force, std::string& ownerPubOut, bool& createdOut, std::string& err);
    // Import "<priv hex>:<pub hex>" — the pair another front-end (or an earlier install) made.
    // The two halves must belong together (checked by a seal/open round trip).
    bool importKey(const std::string& privColonPub, std::string& ownerPubOut, std::string& err);
    // Learn the agent: card = its Agent Card as JSON text, or a path to a file holding it.
    // relayUrl empty keeps the stored relay (default http://127.0.0.1:8645).
    bool pair(const std::string& cardJsonOrPath, const std::string& accountId,
              const std::string& relayUrl, std::string& err);
    // Key + pairing present? `why` names the missing step.
    bool ready(std::string& why) const;
    bool hasKey() const;
    bool isPaired() const;

    // Sign, seal, publish one line of text. nonceOut = the nonce used; sealedBytesOut = size.
    bool send(const std::string& text, int64_t& nonceOut, size_t& sealedBytesOut, std::string& err);
    // Every reply on the topic since sinceNs that this owner's key opens and that was not seen
    // before, oldest first. false = the relay did not answer (err says how).
    bool poll(int64_t sinceNs, std::vector<OwnerReply>& out, std::string& err);

    // What this client knows, as JSON, never a secret: initialised, paired, owner_pub,
    // agent_name, agent_key, account, topic, relay, last_nonce, state (the file path).
    std::string statusJson() const;
    // Single string fields for the console client's `status`.
    std::string field(const char* key) const;
    int64_t lastNonce() const;

    // ---- pure pieces: no network, no state file; `pilot-owner selftest` exercises them ----

    // The signed envelope exactly as the agent verifies it (PilotImpl::verifyOwnerMessage):
    // canonical bytes = compact JSON of the envelope with _logos.signature absent and
    // _logos.signing_key present; signature = ECDSA-secp256k1 over SHA-256 of those bytes.
    static std::string buildEnvelope(const std::string& text, const ECIESKeypair& owner, int64_t nonce);
    // The agent's check, reproduced: verify the signature and hand back message + signing key.
    static bool verifyEnvelope(const std::string& raw, std::string& messageOut, std::string& keyOut);
    // Nonces are the wall clock in ms, strictly increasing: a lost state file never replays.
    static int64_t nextNonce(int64_t lastNonce, int64_t nowMs);
    // The owner topic for an agent account id.
    static std::string topicFor(const std::string& accountId);
    // Seal an envelope to the agent's signing key (ECIES, serialized). Throws on a bad key.
    static std::string sealForAgent(const std::string& envelope, const std::string& agentKeyHex);
    // Parse a relay store body ({"messages":[{"messageHash":..,"message":{"payload":<b64>,
    // "timestamp":<ns>}}]}), open every payload the owner key opens, skip hashes already in
    // `seen` and append the new ones to it. Payloads sealed to someone else (our own sends) are
    // silently skipped — that is how the two directions share one topic.
    static std::vector<OwnerReply> repliesFromStoreBody(const std::string& body,
                                                        const std::string& ownerPrivHex,
                                                        std::vector<std::string>& seen);
    // The store query URL for a topic since sinceNs (page 100, oldest first, payloads included).
    static std::string storeUrl(const std::string& relay, const std::string& topic, int64_t sinceNs);

    static int64_t nowMs();
    static int64_t nowNs();

private:
    std::string dir_;
};
