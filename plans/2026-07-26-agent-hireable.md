# Agent Hireable Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make an agent actually hireable by a stranger — it listens on the address its own Agent Card advertises, stores cards that arrive, and the paid-task loop is either proven end to end or its remaining blocker is named with evidence.

> **DECIDED 2026-07-27 (Johnnie).** The open question at the top of the handoff — does an agent
> open itself for business automatically, or only when told — was answered: **only when told.**
> Three follow-ups settled the shape of it:
>
> 1. **Remembered across restarts.** The owner says it once; a reboot comes back open. A per-boot
>    decision would mean every restart, crash or redeploy silently drops the agent off the market
>    — the same class of failure this plan exists to kill, with a different trigger.
> 2. **Closed is not deaf.** The shared discovery channel stays subscribed either way, so a closed
>    agent still learns who else is out there and can hire *them*. Only its own inbox goes quiet.
> 3. **A closed agent can still read its own card,** it just is not broadcast, and the card says
>    `_logos.open_for_hire: false` so the state is legible rather than inferred.
>
> This makes "advertising but not listening" representable again, which is precisely the bug being
> fixed — so the guard is load-bearing: **`agentCard()` only broadcasts while open for hire**, and
> the same flag gates the inbox subscription, so the two can never disagree. Task 2 below is
> rewritten accordingly; Task 1's tests pin *both* halves of the invariant (open ⇒ listening,
> closed ⇒ not listening and not broadcasting).

**Architecture:** One defined boot order replaces today's accidental one. An agent's inbox topics contain its own keys, so they can only be subscribed *after* an identity exists — but the subscribe currently lives inside `initDeliveryModule()`, which fires before identity load, finds empty keys, skips both subscribes, and never retries (run-once guard). We split "what topics does this identity listen on" (pure, testable) from "ask the delivery module for them" (effectful, idempotent), call the latter from `initialize()` and again from `agentCard()` so publishing a card can never advertise an address we are not listening on, and add the missing discovery-topic branch so a card that arrives live is stored instead of dropped.

**Tech Stack:** C++17 Logos module (Qt Remote Objects, SQLite), bash integration tests, Nix builds.

## Global Constraints

- **Pure C++ in `pilot_impl.h`** — `std::string`, `int64_t`, `std::vector<T>` only. No Qt types in the universal header. Qt types are fine inside `.cpp`.
- **Module unit tests:** `cd pilot-module && nix build .#unit-tests -L` with the Cachix substituters below. Tests live in `pilot-module/tests/` and MUST be registered in `pilot-module/tests/CMakeLists.txt` under `TEST_SOURCES`.
- **Cachix is not optional.** Without `--extra-substituters https://logos-pilot-johnniedom.cachix.org --extra-trusted-public-keys logos-pilot-johnniedom.cachix.org-1:xRlS8BbvLyiZk3ydgRHbSpRVyz1y2M/xMadZ8d15jb0=` nix compiles the whole Rust wallet stack from source (hours on this box). Use `~/build-module-tests.sh`.
- **Never pipe a build or test through `tail`** — a failing assertion scrolls away.
- **Long builds must be detached** — this harness reaps background tasks after ~10 min. `~/build-lgx-detached.sh` starts, poll `~/lgx-build.log` for `LGX_BUILD_EXIT=`.
- **Driving WSL from a Windows session:** always a script FILE (`wsl -d Ubuntu -- bash /home/johnnie/x.sh`), prefixed `MSYS_NO_PATHCONV=1` when invoked from Git Bash. A `|` inside a quoted pattern gets re-interpreted across the hop — put patterns in script files.
- **A module change only takes effect once reinstalled.** `~/build-pilot-lgx.sh` then `~/install-fresh-module.sh`. Verify with `~/verify-module-methods.sh` — `strings` on the `.so` is NOT a valid check (these are Qt meta-object methods).
- **Sequencer:** `~/seq-boot.sh` boots it with `KEEP_STATE=1`. Never wipe the chain — it invalidates the funded demo wallet in `~/.pilot`. Takes ~140s to answer on :3040.
- **Never label something fixed unless it was run and observed.** This plan exists because a test suite reported green over a path that had never once worked.
- **A test asserting "the call returned" is not a test.**

## File Structure

| File | Responsibility |
| --- | --- |
| `pilot-module/src/pilot_impl.h` | Declares `identityTopics()` (pure) and `subscribeIdentityTopics()` (effectful). Pure C++ signatures. |
| `pilot-module/src/pilot_impl.cpp` | Implements both; removes the identity-dependent subscribes from `initDeliveryModule()`; adds the discovery-topic branch to the `messageReceived` callback. |
| `pilot-module/src/pilot_identity.cpp` | Calls delivery bring-up + `subscribeIdentityTopics()` from `initialize()`, on both the load-existing and create-fresh identity paths. |
| `pilot-module/src/pilot_a2a.cpp` | `agentCard()` calls `subscribeIdentityTopics()` before returning, so advertising implies listening. |
| `pilot-module/tests/test_boot_order.cpp` | New. Asserts the advertise/listen invariant and discovery-topic inclusion. |
| `pilot-module/tests/CMakeLists.txt` | Registers the new test file. |
| `test-two-agents-docker.sh` | Asserts from each agent's OWN log that it subscribed to the address its card advertises — the assertion that would have caught this bug. |
| `KNOWN_LIMITATIONS.md` | §7 updated to whatever the end-to-end run actually shows. |

---

### Task 1: The advertise/listen invariant, and the boot-order fix

The bug: `initDeliveryModule()` (`pilot_impl.cpp:237`) brings up the Waku node and then subscribes the agent's own inbox topics at lines 275–281. It is guarded by `deliveryInitialized_` to run once, and it fires before the identity is loaded — Agent B ran it at 15:26:51, before `initialize()` completed. At that moment `a2aSelfEncKey()` and `agentEciesPub_` are empty, so `if (!encKey.empty())` skips both subscribes, and the run-once guard means it never tries again. The agent then publishes a card naming an address it is not listening on. Measured 2026-07-26: B's advertised inbox `04820a35…` appears in B's log exactly twice, both times as filter noise at the moment A's task arrived; B's only content-topic subscription was one it picked up incidentally by *sending*.

**Files:**
- Modify: `pilot-module/src/pilot_impl.h` (declarations + one member)
- Modify: `pilot-module/src/pilot_impl.cpp:237-281` (split the subscribe out of `initDeliveryModule`)
- Create: `pilot-module/tests/test_boot_order.cpp`
- Modify: `pilot-module/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `std::string a2aSelfEncKey()` (existing, `pilot_a2a.cpp`), member `agentEciesPub_`, `std::string agentCard()` (existing).
- Produces:
  - `std::vector<std::string> PilotImpl::identityTopics()` — every topic this identity must listen on: the enc-key inbox, the legacy signing-key inbox when it differs, and `/pilot/1/discovery/proto`. Empty keys contribute no inbox entry. Task 2 and Task 3 rely on this exact name and return type.
  - `void PilotImpl::subscribeIdentityTopics()` — asks `delivery_module` to subscribe every `identityTopics()` entry not already asked for. Idempotent. Task 2 calls it.

- [ ] **Step 1: Write the failing test**

Create `pilot-module/tests/test_boot_order.cpp`:

```cpp
#include <logos_test.h>
#include "../src/pilot_impl.h"
#include "../src/pilot_crypto.h"
#include <sqlite3.h>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QByteArray>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// An agent that advertises an address it does not listen on is unhireable, and that is
// exactly what shipped: initDeliveryModule() subscribed the inbox topics BEFORE the
// identity existed, so both subscribes were skipped and never retried (measured
// 2026-07-26 — the peer's own advertised inbox never appears as a subscription in its log).
// These tests pin the invariant: whatever key the card advertises, the agent listens there.
//
// HONEST LIMIT: this proves the topic LIST is right, which is a pure question and the part
// a unit test can own. It does NOT prove the delivery module accepted the subscription —
// no test in this suite mocks delivery_module. Phase 8 of test-two-agents-docker.sh
// asserts the real subscription from a live agent's own log.

static std::string bootDir(const std::string& name) {
    std::string base = "/tmp";
    if (const char* t = std::getenv("TMPDIR")) base = t;
    std::string dir = base + "/pilot_boot_" + name;
    std::remove((dir + "/pilot.db").c_str());
    std::remove((dir + "/pilot.db-wal").c_str());
    std::remove((dir + "/pilot.db-shm").c_str());
    return dir;
}

// Seed an identity the way a previous boot would have left one, so loadIdentity() restores
// it. Key names are the ones the module really uses (verified in a live agent's config
// table): enc.pub/enc.priv is the dedicated encryption pair, ecies.pub/ecies.priv the
// legacy signing pair.
static void seedIdentity(const std::string& dir,
                         const ECIESKeypair& enc, const ECIESKeypair& ecies) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_exec(db,
        "INSERT OR REPLACE INTO agent_identity (id, npk, account_id) "
        "VALUES (1, '{\"nullifier_public_key\":\"aa\",\"viewing_public_key\":\"bb\"}', 'acct-1');",
        nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?);", -1, &st, nullptr);
    const std::pair<const char*, std::string> rows[] = {
        {"enc.pub",    enc.publicKeyHex},   {"enc.priv",    enc.privateKeyHex},
        {"ecies.pub",  ecies.publicKeyHex}, {"ecies.priv",  ecies.privateKeyHex},
    };
    for (const auto& r : rows) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, r.first, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, r.second.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Read discovered_agents straight out of the DB, the same way test_a2a_outbound.cpp
// inspects its tables. There is no routing-key test accessor on PilotImpl — do not invent
// one; the stored row IS the observable fact.
static bool cardStored(const std::string& dir, const std::string& npk) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM discovered_agents WHERE npk = ?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n > 0;
}

// THE invariant. Take the key the card publishes as _logos.enc_key — the key a buyer
// encrypts to and the address a buyer sends work to — and require that the agent's own
// listen list contains that exact inbox topic.
LOGOS_TEST(listens_on_the_address_its_card_advertises) {
    std::string dir = bootDir("advertise_equals_listen");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }        // create the schema
    seedIdentity(dir, enc, ecies);

    PilotImpl impl; impl.initialize(dir);            // loadIdentity() restores the keys
    QJsonObject card = QJsonDocument::fromJson(
        QByteArray::fromStdString(impl.agentCard())).object();
    std::string advertised = card["_logos"].toObject()["enc_key"].toString().toStdString();

    LOGOS_ASSERT_TRUE(!advertised.empty());
    LOGOS_ASSERT_TRUE(contains(impl.identityTopics(),
                               "/pilot/1/inbox-" + advertised + "/proto"));
}

// The legacy signing-key inbox is also listened on, so a pre-split peer that still routes
// to _logos.signing_key keeps reaching us. Both inboxes, not one.
LOGOS_TEST(listens_on_the_legacy_signing_key_inbox_too) {
    std::string dir = bootDir("legacy_inbox");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedIdentity(dir, enc, ecies);

    PilotImpl impl; impl.initialize(dir);
    std::vector<std::string> topics = impl.identityTopics();
    LOGOS_ASSERT_TRUE(contains(topics, "/pilot/1/inbox-" + enc.publicKeyHex + "/proto"));
    LOGOS_ASSERT_TRUE(contains(topics, "/pilot/1/inbox-" + ecies.publicKeyHex + "/proto"));
}

// The shared discovery channel is part of the identity listen set, not a thing only
// agentDiscover() subscribes when polled — otherwise a card broadcast between polls is
// never even offered to us.
LOGOS_TEST(listens_on_the_discovery_topic) {
    std::string dir = bootDir("discovery_topic");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedIdentity(dir, enc, ecies);

    PilotImpl impl; impl.initialize(dir);
    LOGOS_ASSERT_TRUE(contains(impl.identityTopics(), "/pilot/1/discovery/proto"));
}

// No identity yet -> no inbox topics invented. An agent with no keys must not claim to
// listen on "/pilot/1/inbox-/proto", which is what naive string building would produce.
LOGOS_TEST(no_identity_yields_no_inbox_topics) {
    std::string dir = bootDir("no_identity");
    PilotImpl impl; impl.initialize(dir);            // fresh dir, no wallet -> no identity
    for (const std::string& t : impl.identityTopics())
        LOGOS_ASSERT_TRUE(t.find("/pilot/1/inbox-/proto") == std::string::npos);
}

// Idempotent: asking twice must not queue a second subscription for the same topic.
LOGOS_TEST(subscribe_identity_topics_is_idempotent) {
    std::string dir = bootDir("idempotent");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedIdentity(dir, enc, ecies);

    PilotImpl impl; impl.initialize(dir);
    std::vector<std::string> first = impl.identityTopics();
    impl.subscribeIdentityTopics();
    impl.subscribeIdentityTopics();
    LOGOS_ASSERT_EQ(impl.identityTopics().size(), first.size());
}
```

- [ ] **Step 2: Register the test file**

In `pilot-module/tests/CMakeLists.txt`, add to `TEST_SOURCES` after `test_agent_card.cpp`:

```cmake
        test_boot_order.cpp
```

- [ ] **Step 3: Run it and watch it fail**

Run: `bash ~/build-module-tests.sh`
Expected: FAIL to compile — `identityTopics` and `subscribeIdentityTopics` are not members of `PilotImpl`. Read `~/module-build-full.log` for the error, do not pipe through `tail`.

- [ ] **Step 4: Declare both in the universal header**

In `pilot-module/src/pilot_impl.h`, beside the other A2A members:

```cpp
    // Every topic this identity must listen on: the enc-key inbox (the address the card
    // advertises as _logos.enc_key), the legacy signing-key inbox when it differs, and the
    // shared discovery channel. PURE — no I/O — so the advertise/listen invariant is
    // testable without a delivery module. Empty keys contribute no inbox entry: an agent
    // with no identity must never claim to listen on "/pilot/1/inbox-/proto".
    std::vector<std::string> identityTopics();

    // Ask delivery_module to subscribe every identityTopics() entry not already asked for.
    // Idempotent, so it is safe to call on every path that could be the first one to run
    // after an identity appears. This CANNOT live in initDeliveryModule(): that runs before
    // the identity is loaded and only once, which is why an agent advertised an address it
    // was not listening on (2026-07-26).
    void subscribeIdentityTopics();
```

And beside the other members:

```cpp
    std::vector<std::string> subscribedTopics_;   // topics already asked for (idempotency)
```

- [ ] **Step 5: Implement both, and take the subscribes out of `initDeliveryModule`**

In `pilot-module/src/pilot_impl.cpp`, DELETE these lines from `initDeliveryModule()` (currently 269–281 — the comment block plus both `subscribe` calls):

```cpp
        // A2A server: also listen on our own inbox(es) so peer agents can send us tasks. L1 key
        // separation: the PRIMARY inbox is keyed on the dedicated ENCRYPTION key (a2aSelfEncKey(),
        // advertised as _logos.enc_key) — the key new peers encrypt to. We ALSO subscribe the
        // legacy SIGNING-key inbox (agentEciesPub_) when it differs, so a pre-split peer that still
        // routes to _logos.signing_key keeps reaching us (a2aTryDecrypt then decrypts via the ecies
        // fallback). This dual subscribe is load-bearing for the new<->new two-agent pay loop.
        std::string encKey = a2aSelfEncKey();
        if (!encKey.empty())
            delivery->invokeRemoteMethod("delivery_module", "subscribe",
                QString::fromStdString("/pilot/1/inbox-" + encKey + "/proto"), Timeout(15000));
        if (!agentEciesPub_.empty() && agentEciesPub_ != encKey)
            delivery->invokeRemoteMethod("delivery_module", "subscribe",
                QString::fromStdString("/pilot/1/inbox-" + agentEciesPub_ + "/proto"), Timeout(15000));
```

Then add both functions immediately after `initDeliveryModule()`:

```cpp
// The topics whose NAMES depend on our own identity keys. Kept pure and separate from the
// subscribing so the one invariant that matters — we listen where our card says we listen —
// can be asserted without a delivery module (nothing in this suite mocks one).
//
// L1 key separation: the PRIMARY inbox is keyed on the dedicated ENCRYPTION key
// (a2aSelfEncKey(), advertised as _logos.enc_key) — the key new peers encrypt to. The legacy
// SIGNING-key inbox (agentEciesPub_) is also listened on when it differs, so a pre-split peer
// still routing to _logos.signing_key keeps reaching us (a2aTryDecrypt handles either).
std::vector<std::string> PilotImpl::identityTopics() {
    std::vector<std::string> topics;
    const std::string encKey = a2aSelfEncKey();
    if (!encKey.empty())
        topics.push_back("/pilot/1/inbox-" + encKey + "/proto");
    if (!agentEciesPub_.empty() && agentEciesPub_ != encKey)
        topics.push_back("/pilot/1/inbox-" + agentEciesPub_ + "/proto");
    // The shared channel every Agent Card is published to. Listening here is what lets a
    // card broadcast BETWEEN discovery polls reach us at all.
    topics.push_back("/pilot/1/discovery/proto");
    return topics;
}

void PilotImpl::subscribeIdentityTopics() {
    auto* delivery = logosAPI_ ? logosAPI_->getClient("delivery_module") : nullptr;
    if (!delivery || !delivery->isConnected()) return;

    for (const std::string& topic : identityTopics()) {
        if (std::find(subscribedTopics_.begin(), subscribedTopics_.end(), topic)
                != subscribedTopics_.end())
            continue;
        delivery->invokeRemoteMethod("delivery_module", "subscribe",
            QString::fromStdString(topic), Timeout(15000));
        subscribedTopics_.push_back(topic);
    }
}
```

Add `#include <algorithm>` to `pilot_impl.cpp` if it is not already included.

- [ ] **Step 6: Run the tests**

Run: `bash ~/build-module-tests.sh`
Expected: PASS, including all five new tests. If `agentCard()` returns an empty `_logos.enc_key` under the test harness, the first test fails on its `!advertised.empty()` assertion — that is a real finding about `agentCard()`, not a test bug. Record it and stop rather than weakening the assertion.

- [ ] **Step 7: Commit**

```bash
git add pilot-module/src/pilot_impl.h pilot-module/src/pilot_impl.cpp \
        pilot-module/tests/test_boot_order.cpp pilot-module/tests/CMakeLists.txt
git commit -m "fix(a2a): listen on the address the card advertises"
```

---

### Task 2: Open for hire — the owner's decision, remembered

> **REWRITTEN 2026-07-27** after the decision recorded at the top of this plan. The original
> Task 2 called `subscribeIdentityTopics()` unconditionally from `initialize()`, which opened
> every agent for business the moment it had keys. Johnnie chose explicit owner control, so the
> subscribe is gated on a remembered flag and `agentCard()` is gated on the same flag.

Task 1 built the mechanism; nothing calls it yet, so the agent still listens nowhere. Wire it into the moments that matter: when an identity exists (restoring the owner's standing decision), when the owner opens or closes the agent, and whenever a card would be published.

**Files:**
- Modify: `pilot-module/src/pilot_identity.cpp` (`initialize()`, both success paths)
- Modify: `pilot-module/src/pilot_a2a.cpp` (`agentCard()`, before the return)
- Modify: `pilot-module/src/pilot_impl.cpp` / `pilot_impl.h` (delete the dead `initDependencyModules()`)

**Interfaces:**
- Consumes: `identityTopics()`, `subscribeIdentityTopics()` (Task 1), `initDeliveryModule()` (existing).
- Produces: no new signatures. After `initialize()` returns true, the agent has asked to listen on every `identityTopics()` entry.

- [x] **Step 1: The remembered flag**

`pilot_impl.h` gains `bool openForHire_ = false;` and `bool persistOpenForHire(bool);`.
`pilot_identity.cpp`'s `loadIdentity()` config loop restores it:

```cpp
                else if (key == "a2a.open_for_hire") openForHire_ = (val == "1");
```

Absent means closed — an agent that was never told to open stays off the market.

- [x] **Step 2: The two owner commands**

`pilot_a2a.cpp` gains `agentOpenForHire()`, `agentCloseForHire()`, `agentIsOpenForHire()`.
Opening persists the flag, brings delivery up and subscribes. Closing persists and re-runs
`subscribeIdentityTopics()`, which now also **unsubscribes** topics that left the set — so
closing actually stops strangers reaching the inbox rather than merely un-advertising it.

- [x] **Step 3: Wire it into `initialize()`**

In `pilot_identity.cpp`, both the `loadIdentity()` and `createIdentity()` success branches call
`initDeliveryModule(); subscribeIdentityTopics();`. What that subscribes depends on the restored
flag: the discovery channel always, the agent's own inbox(es) only if the owner had already
opened it. So "I opened my agent" survives the reboot and a fresh agent comes back silent.

- [x] **Step 4: Never advertise an address we are not listening on**

In `pilot_a2a.cpp`, `agentCard()` still *builds* the card while closed (the owner can inspect
their own identity without that being the same act as offering to work for strangers), stamps
`_logos.open_for_hire`, and broadcasts **only** when open — re-asserting the subscription first,
so a card can never go out ahead of the listen.

- [ ] **Step 3: Delete the dead `initDependencyModules()`**

Nothing calls it — `grep -rn "initDependencyModules" pilot-module/src` finds only its definition (`pilot_impl.cpp:329`) and its declaration (`pilot_impl.h:219`). It looks like the intended startup hook, which is misleading now that startup is explicit. Delete both, and verify nothing else refers to it:

```bash
grep -rn "initDependencyModules" pilot-module/ && echo "STILL REFERENCED - do not delete" || echo "clear"
```

If anything outside `pilot_impl.h`/`pilot_impl.cpp` references it (including generated Qt glue under a build directory), leave it in place and note why instead.

- [ ] **Step 4: Run the tests**

Run: `bash ~/build-module-tests.sh`
Expected: PASS — all 163 existing tests plus Task 1's five. A failure here most likely means `initialize()` now reaches delivery code that the harness has no module for; the guard `if (!delivery || !delivery->isConnected()) return;` in `subscribeIdentityTopics()` should absorb that, so investigate rather than deleting the call.

- [ ] **Step 5: Commit**

```bash
git add pilot-module/src/pilot_identity.cpp pilot-module/src/pilot_a2a.cpp \
        pilot-module/src/pilot_impl.cpp pilot-module/src/pilot_impl.h
git commit -m "fix(a2a): subscribe the agent's own inboxes once an identity exists"
```

---

### Task 3: Store a card that arrives instead of dropping it

The `messageReceived` callback (`pilot_impl.cpp:285-325`) has exactly three branches — our inbox, `/pilot/1/reply-…`, and the owner channel — and returns early on anything else. A card arriving live on the discovery topic is therefore read off the wire and thrown away. Measured 2026-07-26: Agent A received one `contentTopic=/pilot/1/discovery/proto` message and stored nothing from it. That leaves `storeQuery` as the only path a card can ever take, and there is no store/archive service in this topology, which is why discovery returns `{"count":0}` forever.

**Files:**
- Modify: `pilot-module/src/pilot_impl.cpp` (the `messageReceived` lambda)
- Modify: `pilot-module/tests/test_boot_order.cpp` (add the caching test)

**Interfaces:**
- Consumes: `bool a2aCacheDiscoveredCard(sqlite3* db, const QJsonObject& card, const std::string& topic, const std::string& lastSeen)` (existing, `pilot_a2a.cpp`) — verifies the signature, TOFU-pins on first contact, and refuses to let a non-valid card evict a validated row. `std::string nowTimestamp()` (existing).
- Produces: no new signatures. A card arriving on the discovery topic is stored under the same rules as an imported or store-queried one.

- [ ] **Step 1: Write the failing test**

Append to `pilot-module/tests/test_boot_order.cpp`:

```cpp
// A card that arrives live on the discovery topic must be STORED, under exactly the same
// verification an imported card gets. Before this, cards were received and dropped, so
// discovery could only ever work through a store/archive service that this topology does
// not have (measured 2026-07-26: one discovery message received, nothing stored).
//
// handleDiscoveryCard() is the seam the messageReceived callback calls, so it can be tested
// without a delivery module.
LOGOS_TEST(a_card_arriving_on_the_discovery_topic_is_stored) {
    std::string dir = bootDir("live_card_cached");
    { PilotImpl boot; boot.initialize(dir); }
    PilotImpl impl; impl.initialize(dir);

    // A genuine self-signed card: signed by its own published identity key, so
    // verifyCardStatus() == "valid" and a2aCacheDiscoveredCard() accepts it.
    ECIESKeypair kp = generateECIESKeypair();
    QJsonObject logos;
    logos["npk"] = QString("npk-live-peer");
    logos["signing_key"] = QString::fromStdString(kp.publicKeyHex);
    logos["enc_key"] = QString::fromStdString(kp.publicKeyHex);
    QJsonObject card;
    card["name"] = QString("Pilot Agent");
    card["_logos"] = logos;
    std::string canonical = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    QJsonObject sig;
    sig["alg"] = QString("ES256K");
    sig["publicKey"] = QString::fromStdString(kp.publicKeyHex);
    sig["value"] = QString::fromStdString(signMessage(bytes, kp.privateKeyHex));
    card["signature"] = sig;

    impl.handleDiscoveryCard(
        QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString());

    // Stored under its payment identity — which is what makes it routable and payable.
    LOGOS_ASSERT_TRUE(cardStored(dir, "npk-live-peer"));
}

// Junk on the discovery topic must be ignored quietly, never stored and never fatal.
LOGOS_TEST(junk_on_the_discovery_topic_is_ignored) {
    std::string dir = bootDir("live_card_junk");
    { PilotImpl boot; boot.initialize(dir); }
    PilotImpl impl; impl.initialize(dir);

    impl.handleDiscoveryCard("not json");
    impl.handleDiscoveryCard("{\"name\":\"no logos block\"}");
    LOGOS_ASSERT_TRUE(!cardStored(dir, "npk-live-peer"));
}
```

`signMessage` and `generateECIESKeypair` come from `pilot_crypto.h`, already included at the top of the file. `cardStored()` is the helper added in Step 1 above — there is no routing-key test accessor on `PilotImpl`, so do not reach for one.

- [ ] **Step 2: Run it and watch it fail**

Run: `bash ~/build-module-tests.sh`
Expected: FAIL to compile — `handleDiscoveryCard` is not a member of `PilotImpl`.

- [ ] **Step 3: Declare it**

In `pilot-module/src/pilot_impl.h`, beside `agentImportCard`:

```cpp
    // A card that arrived on the shared discovery topic. Runs the SAME verification and TOFU
    // pinning as an imported card — only the delivery differs. Junk is ignored quietly: this
    // is a public channel and anyone can publish to it.
    void handleDiscoveryCard(const std::string& cardJson);
```

- [ ] **Step 4: Implement it**

In `pilot-module/src/pilot_a2a.cpp`, directly above `agentImportCard`:

```cpp
void PilotImpl::handleDiscoveryCard(const std::string& cardJson) {
    if (!db_) return;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(cardJson));
    if (!doc.isObject()) return;
    QJsonObject card = doc.object();
    if (card["_logos"].toObject()["npk"].toString().isEmpty()) return;
    // Same store, same signature check, same TOFU pin, same no-evict guard as every other
    // way a card can reach us. A public channel earns no extra trust and no less.
    a2aCacheDiscoveredCard(db_, card, "/pilot/1/discovery/proto", nowTimestamp());
}
```

- [ ] **Step 5: Add the missing branch to the callback**

In `pilot-module/src/pilot_impl.cpp`, inside the `messageReceived` lambda, immediately after the `/pilot/1/reply-` branch and before the owner-channel check:

```cpp
                    // A peer's Agent Card broadcast on the shared discovery channel. Without
                    // this branch the message was received and dropped, so a card could only
                    // ever be learned by store query (no archive service here) or by
                    // out-of-band import.
                    if (topic == "/pilot/1/discovery/proto") {
                        handleDiscoveryCard(payload);
                        return;
                    }
```

- [ ] **Step 6: Run the tests**

Run: `bash ~/build-module-tests.sh`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add pilot-module/src/pilot_impl.h pilot-module/src/pilot_impl.cpp \
        pilot-module/src/pilot_a2a.cpp pilot-module/tests/test_boot_order.cpp
git commit -m "feat(a2a): store a peer card that arrives on the discovery topic"
```

---

### Task 4: The assertion that would have caught this

Every unit test above asserts intent. None proves a live agent asked the network for anything — and that gap is precisely where this bug lived for weeks. Assert it from the agent's own log.

**Files:**
- Modify: `test-two-agents-docker.sh` (new Phase 8, before the results block)

**Interfaces:**
- Consumes: `check_has()`, `call_a`, `call_b`, `$AGENT_A`, `$CONTAINER` (all existing in that script).
- Produces: a phase that fails when an agent advertises an address it is not listening on.

- [ ] **Step 1: Write the failing phase**

In `test-two-agents-docker.sh`, insert immediately before the `echo "══…"` results block:

```bash
# ═══════════════════════════════════════
echo "── Phase 8: Agents listen where their cards say ──"

# Every unit test in the module asserts the topic LIST. Only this asserts that a live agent
# actually asked the network. The gap between those two is where the unhireable-agent bug
# lived: the card went out, the inbox was never subscribed, and nothing anywhere noticed.
for who in A B; do
  if [ "$who" = "A" ]; then
    CARD=$(call_a agentCard); LOG_TXT=$(cat "$AGENT_A/daemon.log" 2>/dev/null)
  else
    CARD=$(call_b agentCard); LOG_TXT=$(docker logs $CONTAINER 2>&1)
  fi
  ENC=$(echo "$CARD" | python3 -c \
    'import sys,json;print(json.load(sys.stdin)["_logos"].get("enc_key",""))' 2>/dev/null)
  if [ -z "$ENC" ]; then
    echo "  FAIL  [$who] card has no _logos.enc_key to check"; ((FAIL++)); continue
  fi
  # A "subscribe" line naming the advertised inbox. Filter-service noise
  # ("no subscribed peers found") is NOT evidence of our own subscription, so require a
  # line that is about subscribing AND names our topic.
  HITS=$(echo "$LOG_TXT" | grep -F "/pilot/1/inbox-$ENC/proto" \
          | grep -icE "subscrib" )
  if [ "${HITS:-0}" -ge 1 ]; then
    echo "  PASS  [$who] listens on the inbox its card advertises"; ((PASS++))
  else
    echo "  FAIL  [$who] advertises /pilot/1/inbox-${ENC:0:16}…/proto but never subscribed it"
    ((FAIL++))
  fi
done
echo ""
```

- [ ] **Step 2: Run it against the CURRENT installed module and watch it fail**

Run:
```bash
bash ~/seq-boot.sh
bash ~/a2a-run-detached.sh     # poll ~/a2a-run.log until TEST_EXIT=
```
Expected: Phase 8 FAILS for both agents. That is the bug reproduced by assertion rather than by log archaeology. Record the output.

- [ ] **Step 3: Install the fixed module and rerun**

Run:
```bash
bash ~/build-lgx-detached.sh          # poll ~/lgx-build.log for LGX_BUILD_EXIT=0
bash ~/install-fresh-module.sh
bash ~/verify-module-methods.sh       # agentImportCard present, skill count 23
bash ~/seq-boot.sh
bash ~/a2a-run-detached.sh
```
Expected: Phase 8 PASSES for both agents.

- [ ] **Step 4: Commit**

```bash
git add test-two-agents-docker.sh
git commit -m "test(a2a): assert an agent listens where its card advertises"
```

---

### Task 5: Find out whether a chain operation locks the owner out

**This is a measurement, not a fix.** Its deliverable is a recorded finding that decides whether threading work is needed at all. Do not write a fix in this task.

What is already known and must not be re-derived: LLM calls block on a nested `QEventLoop` (`pilot_llm_anthropic.cpp:47`) that keeps pumping the delivery thread, and the L6 guard (`pilot_impl.cpp:566-568`, `pilot_impl.cpp:615`) refuses a stranger's `agent.ask` while one is in flight but **never refuses the owner** — the owner is privileged by design. So the LLM path is already handled. What is NOT known is whether a wallet call (`invokeRemoteMethod(..., RPC_TIMEOUT)`) pumps the event loop the same way or hard-blocks it. If it hard-blocks, the owner is locked out for the duration of a transfer — minutes in real-proof mode.

**Files:**
- Create: `private/measure-onchain-block.sh` (gitignored, consistent with keeping research out of the submission repo)

- [ ] **Step 1: Write the measurement**

```bash
#!/usr/bin/env bash
# Does an on-chain wallet call freeze the owner's lane, or does it pump like the LLM path?
# Start a transfer, then hit the owner path DURING it and time the answer.
#   answers fast  -> the call pumps; no threading work needed
#   silent/slow   -> hard block; the owner is locked out for the duration
set -uo pipefail
export PATH="$HOME/.nix-profile/bin:/nix/var/nix/profiles/default/bin:$PATH"
export PILOT_DATA_DIR="$HOME/.pilot-measure"
export PILOT_MODULE_PATH="$HOME/.pilot/modules"
export RISC0_DEV_MODE=1
export LOGOS_BLOCKCHAIN_CIRCUITS=$(find /nix/store -maxdepth 1 -name '*logos-blockchain-circuits*' -type d | head -1)
LOGOSCORE=$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f | head -1)
export LOGOS_HOST_PATH=$(find /nix/store -maxdepth 3 -name logos_host -path "*liblogos-bin*" -type f | head -1)
CFG="--config-dir $PILOT_DATA_DIR/.logoscore"

pkill -9 -f logos_host_qt 2>/dev/null; sleep 2
rm -rf "$PILOT_DATA_DIR"; mkdir -p "$PILOT_DATA_DIR"
setsid nohup $LOGOSCORE $CFG -D -m "$PILOT_MODULE_PATH" \
  > "$PILOT_DATA_DIR/daemon.log" 2>&1 < /dev/null &
disown
sleep 6
for m in capability_module logos_execution_zone delivery_module storage_module pilot; do
  timeout 60 $LOGOSCORE $CFG load-module $m >/dev/null 2>&1; sleep 2
done
timeout 300 $LOGOSCORE $CFG call pilot initialize "$PILOT_DATA_DIR" >/dev/null 2>&1
sleep 5
echo "balance: $(timeout 60 $LOGOSCORE $CFG call pilot walletBalance 2>&1 | head -c 120)"

# Fire a transfer in the background, then probe the owner lane while it runs.
( timeout 600 $LOGOSCORE $CFG call pilot walletSend \
    '{"nullifier_public_key":"aa","viewing_public_key":"bb"}' 1 "block probe" \
    > "$PILOT_DATA_DIR/send.out" 2>&1 ) &
SEND_PID=$!
sleep 2
for i in 1 2 3 4 5; do
  START=$(date +%s%N)
  OUT=$(timeout 20 $LOGOSCORE $CFG call pilot echo probe 2>&1 | head -c 60)
  END=$(date +%s%N)
  echo "probe $i: $(( (END-START)/1000000 ))ms  -> ${OUT:-<silence>}"
  sleep 3
done
wait $SEND_PID
echo "send result: $(head -c 200 "$PILOT_DATA_DIR/send.out")"
timeout 10 $LOGOSCORE $CFG stop >/dev/null 2>&1
pkill -9 -f logos_host_qt 2>/dev/null
```

- [ ] **Step 2: Run it with the sequencer up**

Run: `bash ~/seq-boot.sh && bash private/measure-onchain-block.sh`
Expected: five probe timings. Sub-second answers throughout mean the call pumps and no threading work is needed. Probes that hang until the transfer finishes mean a hard block.

- [ ] **Step 3: Record the finding**

Append the observed timings as a comment block at the top of `private/measure-onchain-block.sh`, then add one paragraph to `KNOWN_LIMITATIONS.md` §2 stating which it was. If it hard-blocks, say plainly that a stranger's paid task can stall the owner's chat for the duration of a settlement, and that moving wallet calls off the delivery thread is the fix. Do not write that fix here — it is a separate plan.

- [ ] **Step 4: Commit**

```bash
git add KNOWN_LIMITATIONS.md
git commit -m "docs: measure whether a chain call blocks the owner's lane"
```

---

### Task 6: Diagnose the funded balance that cannot be spent

**This is a diagnosis, not a fix.** Agent A reported `funded=1` and `walletBalance` 100, and `walletSend` failed `Transfer failed: InsufficientFundsError` (2026-07-26, on a kept non-genesis chain). Until this is understood, no paid-task claim can be made regardless of how well the transport works. The cause is unknown, so a fix cannot honestly be written in advance — this task ends at a named cause.

**Files:**
- Create: `private/diagnose-unspendable.sh` (gitignored)

- [ ] **Step 1: Write the diagnosis script**

```bash
#!/usr/bin/env bash
# 100 reported, 0 spendable. Establish WHICH of these is true before designing anything:
#   1. balance is read from a different place than the spendable note set
#   2. the pinata claim landed in the public account and the shielded transfer never ran
#   3. the notes exist but the wallet has not synced far enough to see them
set -uo pipefail
export PATH="$HOME/.nix-profile/bin:/nix/var/nix/profiles/default/bin:$PATH"
D=${1:-/tmp/agent-a}
echo "=== funded flag + account ids in pilot.db ==="
python3 - "$D" <<'PY'
import sqlite3, sys
con = sqlite3.connect("file:%s/pilot.db?mode=ro" % sys.argv[1], uri=True)
for k, v in con.execute("SELECT key, value FROM config;"):
    if any(s in k.lower() for s in ("fund", "account", "public")):
        print("  %-24s %s" % (k, v[:70]))
for r in con.execute("SELECT npk, account_id FROM agent_identity WHERE id=1;"):
    print("  identity npk     %s" % r[0][:70])
    print("  identity account %s" % r[1][:70])
PY
echo
echo "=== wallet_storage.json: accounts and any note/commitment arrays ==="
python3 - "$D" <<'PY'
import json, sys
d = json.load(open("%s/wallet_storage.json" % sys.argv[1]))
def walk(o, path=""):
    if isinstance(o, dict):
        for k, v in o.items():
            if isinstance(v, list):
                print("  %s/%s : list len=%d" % (path, k, len(v)))
            walk(v, path + "/" + k)
    elif isinstance(o, list):
        for i, v in enumerate(o[:3]):
            walk(v, path + "[%d]" % i)
print("top-level keys:", list(d.keys()))
walk(d)
PY
echo
echo "=== the funding steps as the log saw them ==="
grep -inE 'pinata|claim|shield|fund|mint|faucet|PoW' "$D/daemon.log" | tail -30
echo
echo "=== sync position at the moment of the failed send ==="
grep -nE 'Blocks to sync|Synced to block|InsufficientFunds' "$D/daemon.log" | tail -20
```

- [ ] **Step 2: Run it against the failed run's data**

Run: `bash private/diagnose-unspendable.sh /tmp/agent-a`
Expected: one of the three hypotheses supported by evidence. If `/tmp/agent-a` has been wiped by a later run, do a fresh two-agent run first and diagnose that one.

- [ ] **Step 3: Record the cause and STOP**

Write the named cause into `KNOWN_LIMITATIONS.md` §5 (the funding section) with the evidence. Then stop: the fix belongs in a new plan written against a known cause, not guessed at here. If the evidence is inconclusive, say that plainly rather than picking the most likely story.

- [ ] **Step 4: Commit**

```bash
git add KNOWN_LIMITATIONS.md
git commit -m "docs: name why a funded agent cannot spend"
```

---

### Task 7: Run the whole thing and write down what is actually true

**Files:**
- Modify: `KNOWN_LIMITATIONS.md` (§7)
- Modify: `README.md:523`

- [ ] **Step 1: Full run from a clean start**

Run:
```bash
bash ~/seq-boot.sh                    # KEEP_STATE=1, wait for http 200 on :3040
bash ~/install-fresh-module.sh
bash ~/verify-module-methods.sh
bash ~/a2a-run-detached.sh            # poll ~/a2a-run.log until TEST_EXIT=
```

Record the full Phase 2, 5, 7 and 8 output verbatim.

- [ ] **Step 2: Judge it honestly against three separate questions**

- Does discovery now find a peer? (Phase 2 `[B] discovers ≥1 agent`)
- Does a job reach the doer and come back? (Phase 7 task reaches a terminal state, not `submitted`)
- Did money move? (Phase 7 `A balance N -> N-5`)

These can land independently. Two out of three is a real result and must be reported as two out of three.

- [ ] **Step 3: Rewrite §7 to match what was observed**

`KNOWN_LIMITATIONS.md` §7 currently says the loss is on the receiving side between the node and the module, with the exact seam unpinned. That is now pinned: the agent never subscribed to its own inbox. Rewrite it as what was wrong, what fixed it, and what is still open — with the run output as evidence. If the paid loop still does not complete, §7 must still say so.

- [ ] **Step 4: Fix the README overclaim**

`README.md:523` says "Agent A publishes Agent Card, Agent B discovers it via Waku store". Correct it to whatever the run in Step 1 actually shows. If discovery works live but not via store query, say live relay. If it needed an imported card, say that.

- [ ] **Step 5: Commit**

```bash
git add KNOWN_LIMITATIONS.md README.md
git commit -m "docs: record what the two-agent paid loop actually does"
```

---

## Verification (the whole point)

The plan is done when a single run prints all four of these, from a real two-agent run:

```
── Phase 2: Agent Discovery ──
  PASS  [B] discovers ≥1 agent
── Phase 7: A2A Paid Task (the headline claim) ──
  PASS  [A→B] paid task accepted
  PASS  [A] task was PAID
  A balance 100 -> 95
── Phase 8: Agents listen where their cards say ──
  PASS  [A] listens on the inbox its card advertises
  PASS  [B] listens on the inbox its card advertises
```

Tasks 1–4 are expected to deliver Phase 8 and the discovery half. **Task 6 is the gate on `A balance 100 -> 95`,** and it is a diagnosis with an unknown answer — so this final block may not be reachable within this plan. If it is not, say which line is missing and why, and do not claim the loop works.
