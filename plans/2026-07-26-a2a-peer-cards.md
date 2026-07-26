# A2A Peer Cards Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make agent-to-agent tasks and payments work whether or not broadcast discovery delivers, by letting an agent learn a peer from an imported Agent Card, and prove with a test that LEZ actually moves.

**Architecture:** One peer store, two ways in. Cards already arrive from the network via `agentDiscover` → `a2aCacheDiscoveredCard()`, which verifies the signature, TOFU-pins the identity and refuses to let a non-valid card evict a validated row. A new `agentImportCard()` feeds the *same* function from a file or paste, so an out-of-band card is neither more nor less trusted than a broadcast one. Everything downstream (`a2aRoutingKeyFor`, `discoveredPriceFor`, `discoveredPayoutFor`) already reads that store and does not care where a card came from. With import in place, the bare-hex routing fallback that silently dead-drops requests can finally be closed.

**Tech Stack:** C++17 Logos module (Qt Remote Objects, SQLite), Nim CLI, bash integration tests, Nix builds.

## Global Constraints

- **Pure C++ in `pilot_impl.h`** — `std::string`, `int64_t`, `std::vector<T>` only. No Qt types in the universal header; Qt glue is generated. Qt types are fine inside `pilot_a2a.cpp`.
- **Module unit tests:** `nix build .#unit-tests -L` from `pilot-module/`. Tests live in `pilot-module/tests/` and are registered in `pilot-module/tests/CMakeLists.txt`.
- **CLI tests:** `cd pilot-cli && nix --extra-experimental-features 'nix-command flakes' develop -c nim c -r tests/test_rpc.nim`.
- **CLI build:** `cd pilot-cli && nix --extra-experimental-features 'nix-command flakes' build` (PATH needs `$HOME/.nix-profile/bin`).
- **Local module rebuilds may OOM on this box** (4.9 GB WSL). If `nix build` in `pilot-module/` dies, push and let CI build; do not "fix" it by weakening the build.
- **Never label something fixed unless it was run and observed working.** Two false-green tests (`check()` passing `{"count":0}` and passing an `InsufficientFundsError` transfer) are the reason this plan exists.
- **Honesty over green:** a test asserting "the call returned" is not a test.

## File Structure

| File | Responsibility |
| --- | --- |
| `pilot-module/src/pilot_impl.h` | Declares `agentImportCard(const std::string&)`. Pure C++ signature. |
| `pilot-module/src/pilot_a2a.cpp` | Implements `agentImportCard()`; tightens `a2aRoutingKeyFor()`. |
| `pilot-module/src/pilot_builtin_skills.cpp` | Registers the `agent.import_card` skill. |
| `pilot-module/tests/test_agent_card.cpp` | Unit tests for import + the tightened routing fallback (reuses the existing `makeCard()` helper). |
| `pilot-cli/src/rpc.nim` | `readPeerCard()` — load a card from a path or literal JSON, validate shape. Pure, testable. |
| `pilot-cli/pilot.nim` | `pilot peer add <file|json>` / `pilot peer list`. |
| `pilot-cli/src/repl.nim` | `/peer add`, `/peers` in chat. |
| `pilot-cli/tests/test_rpc.nim` | Unit tests for `readPeerCard()`. |
| `test-two-agents-docker.sh` | Phase 7: import card → paid task → assert balances moved. |
| `KNOWN_LIMITATIONS.md` | §7: broadcast discovery unproven; out-of-band import is the reliable path. |

---

### Task 1: Reproduce the agentTask crash with the log preserved

Sending a task killed the `pilot` module process outright (`[critical] Module process crashed: pilot`, 2026-07-26). The log holding the cause was overwritten by a restart before it could be read. Capture it before changing anything — a guard written against a guessed cause is a guess.

**Files:**
- Create: `private/crash-repro.sh`

**Interfaces:**
- Consumes: nothing.
- Produces: a saved log at `private/crash-<timestamp>.log`, and a one-line finding recorded in Task 2's commit message.

- [ ] **Step 1: Write the reproduction script**

```bash
#!/usr/bin/env bash
# Reproduce the agentTask crash with the daemon log PRESERVED.
set -uo pipefail
STAMP=$(date +%Y%m%d-%H%M%S)
export PILOT_DATA_DIR="$HOME/.pilot-crash"
export PILOT_MODULE_PATH="$HOME/.pilot/modules"
export RISC0_DEV_MODE=1
export PILOT_NAT="extip:127.0.0.1"
export LOGOS_BLOCKCHAIN_CIRCUITS=$(find /nix/store -maxdepth 1 -name '*logos-blockchain-circuits*' -type d | head -1)
LOGOSCORE=$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f | head -1)
CFG="--config-dir $PILOT_DATA_DIR/.logoscore"

pkill -f logos_host_qt; sleep 3
rm -rf "$PILOT_DATA_DIR"; mkdir -p "$PILOT_DATA_DIR"
setsid nohup $LOGOSCORE $CFG -D -m "$PILOT_MODULE_PATH" > "$PILOT_DATA_DIR/daemon.log" 2>&1 < /dev/null &
disown
sleep 6
for m in capability_module logos_execution_zone delivery_module storage_module chat_module pilot; do
  timeout 60 $LOGOSCORE $CFG load-module $m >/dev/null 2>&1; sleep 2
done
timeout 120 $LOGOSCORE $CFG call pilot initialize "$PILOT_DATA_DIR" >/dev/null 2>&1
sleep 8

echo "--- alive before: $(timeout 30 $LOGOSCORE $CFG call pilot echo ready 2>&1 | head -c 80)"
echo "--- sending task to an address we have no card for"
timeout 120 $LOGOSCORE $CFG call pilot agentTask \
  '{"agent_address":"02a36ce18bf4221d22f28ee9ee2d5c4e7e5161fabf0995b29eb5cee1ed3e98951d","skill":"agent-ask","params":"{\"prompt\":\"hi\"}"}' 2>&1 | head -c 200
echo
echo "--- alive after : $(timeout 30 $LOGOSCORE $CFG call pilot echo ready 2>&1 | head -c 80)"

cp "$PILOT_DATA_DIR/daemon.log" "$HOME/dev/logos/logos-pilot/private/crash-$STAMP.log"
echo "--- saved: private/crash-$STAMP.log"
grep -nE "crash|terminate|what\(\):|Segmentation|abort|assert" "$PILOT_DATA_DIR/daemon.log" | tail -10
```

- [ ] **Step 2: Run it and read the tail**

Run: `bash private/crash-repro.sh`
Expected: either `alive after:` is empty and the saved log names a cause, or the call returns the `"discover its card first"` error and the module survives (meaning the crash needs a discovered peer to reproduce — record that too).

- [ ] **Step 3: Record the finding**

Append one paragraph to `private/crash-repro.sh`'s header comment stating exactly what was observed, with the log filename. No fix yet.

- [ ] **Step 4: Commit**

```bash
git add private/crash-repro.sh
git commit -m "test(a2a): reproducible harness for the agentTask module crash"
```

---

### Task 2: Guard the task path so an unroutable peer errors instead of dying

Whatever Task 1 found, sending to a peer we cannot route to must return JSON, not take the process down.

**Files:**
- Modify: `pilot-module/src/pilot_a2a.cpp` (`agentTask`, top of body)
- Test: `pilot-module/tests/test_a2a_outbound.cpp`

**Interfaces:**
- Consumes: `a2aRoutingKeyFor(const std::string&)` (existing, returns `std::string`).
- Produces: no signature change — `agentTask` keeps returning a JSON string.

- [ ] **Step 1: Write the failing test**

```cpp
// An address we hold no card for must produce an ERROR OBJECT, never a crash and
// never a silent send. Observed 2026-07-26: the module process died instead.
LOGOS_TEST(agentTask_unroutable_address_returns_error) {
    PilotImpl impl;
    impl.initialize(testDataDir());
    std::string out = impl.agentTask("not-a-key", "agent-ask", "{\"prompt\":\"hi\"}");
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    LOGOS_ASSERT(doc.isObject());
    LOGOS_ASSERT(doc.object().contains("error"));
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd pilot-module && nix build .#unit-tests -L`
Expected: FAIL (no such test / crash / no `error` key).

- [ ] **Step 3: Add the guard**

At the top of `PilotImpl::agentTask`, before any delivery call:

```cpp
    // Fail closed on a peer we cannot route to. Everything below this point
    // subscribes, writes an outbound row and encrypts — work that is worse than
    // useless when the request can only dead-drop.
    if (a2aRoutingKeyFor(agentAddress).empty())
        return "{\"error\": \"no Agent Card for this peer — import one "
               "(pilot peer add <card.json>) or discover it first\"}";
```

- [ ] **Step 4: Run the tests**

Run: `cd pilot-module && nix build .#unit-tests -L`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add pilot-module/src/pilot_a2a.cpp pilot-module/tests/test_a2a_outbound.cpp
git commit -m "fix(a2a): refuse an unroutable task instead of killing the module"
```

---

### Task 3: `agentImportCard` — learn a peer from a card

**Files:**
- Modify: `pilot-module/src/pilot_impl.h` (declaration)
- Modify: `pilot-module/src/pilot_a2a.cpp` (definition, next to `agentDiscover`)
- Test: `pilot-module/tests/test_agent_card.cpp`

**Interfaces:**
- Consumes: `bool a2aCacheDiscoveredCard(sqlite3* db, const QJsonObject& card, const std::string& topic, const std::string& lastSeen)` — existing; verifies, TOFU-pins, refuses to evict a validated row.
- Produces: `std::string PilotImpl::agentImportCard(const std::string& cardJson)` returning
  `{"imported":true,"npk":"<hex>","signature_status":"valid|invalid|unsigned","pricing":{...}}`
  or `{"error":"..."}`. Task 4 and Task 5 call it.

- [ ] **Step 1: Write the failing test**

```cpp
// Import must go through the SAME verification as a broadcast card: a genuine card
// is stored and routable; a card re-signed under a different key is refused.
LOGOS_TEST(agentImportCard_stores_a_genuine_card) {
    PilotImpl impl;
    impl.initialize(testDataDir());
    auto keys = generateKeyPair();                       // pilot_crypto.h
    QJsonObject card = makeCard(keys.publicKey, keys.privateKey, keys.publicKey);
    std::string json = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();

    std::string out = impl.agentImportCard(json);
    QJsonObject res = QJsonDocument::fromJson(QByteArray::fromStdString(out)).object();
    LOGOS_ASSERT(res["imported"].toBool());
    LOGOS_ASSERT(res["signature_status"].toString() == "valid");
    // routable afterwards, which is the whole point
    LOGOS_ASSERT(!impl.a2aRoutingKeyForTest("npk-genuine-identity").empty());
}

LOGOS_TEST(agentImportCard_rejects_malformed_input) {
    PilotImpl impl;
    impl.initialize(testDataDir());
    QJsonObject res = QJsonDocument::fromJson(
        QByteArray::fromStdString(impl.agentImportCard("not json"))).object();
    LOGOS_ASSERT(res.contains("error"));
    res = QJsonDocument::fromJson(
        QByteArray::fromStdString(impl.agentImportCard("{\"name\":\"no logos block\"}"))).object();
    LOGOS_ASSERT(res.contains("error"));
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd pilot-module && nix build .#unit-tests -L`
Expected: FAIL — `agentImportCard` undeclared.

- [ ] **Step 3: Declare it in the universal header**

In `pilot-module/src/pilot_impl.h`, beside the other agent methods:

```cpp
    // Learn a peer from a card handed over out-of-band (file, paste, QR). Runs the
    // SAME signature + TOFU verification as a card received over discovery, so an
    // imported peer is neither more nor less trusted than a broadcast one.
    std::string agentImportCard(const std::string& cardJson);
```

- [ ] **Step 4: Implement it**

In `pilot-module/src/pilot_a2a.cpp`, directly above `agentDiscover`:

```cpp
std::string PilotImpl::agentImportCard(const std::string& cardJson) {
    if (!db_) return "{\"error\": \"not initialized\"}";

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(cardJson));
    if (!doc.isObject())
        return "{\"error\": \"not a JSON object — expected an Agent Card\"}";
    QJsonObject card = doc.object();

    QJsonObject logos = card["_logos"].toObject();
    std::string npk = logos["npk"].toString().toStdString();
    if (npk.empty())
        return "{\"error\": \"card has no _logos.npk — not an Agent Card\"}";

    // Same store, same guard, same TOFU pin as the network path.
    if (!a2aCacheDiscoveredCard(db_, card, "/pilot/1/discovery/proto", nowTimestamp()))
        return "{\"error\": \"refused — a validated card for this identity is already "
               "stored and this one does not verify against it\"}";

    QJsonObject out;
    out["imported"] = true;
    out["npk"] = QString::fromStdString(npk);
    out["signature_status"] = verifyCardStatus(card, db_);
    out["pricing"] = logos["pricing"].toObject();
    return QJsonDocument(out).toJson(QJsonDocument::Compact).toStdString();
}
```

- [ ] **Step 5: Register the skill**

In `pilot-module/src/pilot_builtin_skills.cpp`, beside `agent.discover`:

```cpp
    reg(registry, "agent.import_card", "agent",
        "Imports a peer's signed Agent Card handed over out-of-band", 0,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            return impl->agentImportCard(
                QJsonDocument(doc.object()["card"].toObject()).toJson(QJsonDocument::Compact).toStdString());
        });
```

- [ ] **Step 6: Run the tests**

Run: `cd pilot-module && nix build .#unit-tests -L`
Expected: PASS, and the registered-skill count in `pilot-cli/src/verify.nim`'s `SKILL_NAMES` is now one behind — update that list to include `agent.import_card` and `agent.ask` (it is already missing `agent.ask`, which is why verify prints 21/21 for 22 skills).

- [ ] **Step 7: Commit**

```bash
git add pilot-module/src/pilot_impl.h pilot-module/src/pilot_a2a.cpp \
        pilot-module/src/pilot_builtin_skills.cpp pilot-module/tests/test_agent_card.cpp \
        pilot-cli/src/verify.nim
git commit -m "feat(a2a): import a peer's Agent Card out-of-band"
```

---

### Task 4: CLI — `pilot peer add` / `pilot peer list`, and `/peer` in chat

**Files:**
- Modify: `pilot-cli/src/rpc.nim` (`readPeerCard`)
- Modify: `pilot-cli/pilot.nim` (subcommand + USAGE line)
- Modify: `pilot-cli/src/repl.nim` (`/peer add`, `/peers`, HELP_TEXT)
- Test: `pilot-cli/tests/test_rpc.nim`

**Interfaces:**
- Consumes: `agentImportCard` via `daemonCall(cfg, "agentImportCard", @[cardJson])`.
- Produces: `proc readPeerCard*(pathOrJson: string): tuple[card, err: string]` — used by both the subcommand and the chat command.

- [ ] **Step 1: Write the failing test**

```nim
# A card may be handed over as a file or pasted whole. Validate the shape HERE,
# where the owner can still fix it, not three RPC hops later.
let cardTmp = getTempDir() / "pilot-test-card.json"
const GOOD_CARD = """{"name":"Pilot Agent","_logos":{"npk":"abc123","signing_key":"02aa"}}"""
writeFile(cardTmp, GOOD_CARD & "\n")

doAssert readPeerCard(cardTmp)[0] == GOOD_CARD          # file, trailing newline stripped
doAssert readPeerCard(GOOD_CARD)[0] == GOOD_CARD        # pasted literally
doAssert readPeerCard("/no/such/file.json")[1] != ""    # missing file -> error
doAssert readPeerCard("{\"name\":\"x\"}")[1] != ""      # no _logos.npk -> error
doAssert readPeerCard("not json")[1] != ""
removeFile(cardTmp)
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd pilot-cli && nix --extra-experimental-features 'nix-command flakes' develop -c nim c -r tests/test_rpc.nim`
Expected: FAIL — `readPeerCard` undeclared.

- [ ] **Step 3: Implement `readPeerCard`**

In `pilot-cli/src/rpc.nim`, beside `saveContact`:

```nim
# A peer card arrives as a file path or as the JSON itself. Validate the shape at
# the keyboard, where it can still be fixed, rather than after three RPC hops.
proc readPeerCard*(pathOrJson: string): tuple[card, err: string] =
  var raw = pathOrJson.strip()
  if not raw.startsWith("{"):
    let p = expandTilde(raw)
    if not fileExists(p):
      return ("", "no such card file: " & p)
    raw = readFile(p).strip()
  var parsed: JsonNode
  try: parsed = parseJson(raw)
  except: return ("", "that is not valid JSON — paste the whole card, braces included")
  if parsed.kind != JObject: return ("", "expected a card object")
  let npk = parsed{"_logos", "npk"}.getStr("")
  if npk == "": return ("", "no _logos.npk in that card — is it an Agent Card?")
  return (raw, "")
```

- [ ] **Step 4: Run the tests**

Run: `cd pilot-cli && nix --extra-experimental-features 'nix-command flakes' develop -c nim c -r tests/test_rpc.nim`
Expected: PASS.

- [ ] **Step 5: Wire the subcommand**

In `pilot-cli/pilot.nim`, beside `contact`:

```nim
  of "peer", "peers":
    if rest.len == 0 or (rest.len == 1 and rest[0].toLowerAscii() == "list"):
      echo formatJson(daemonCall(cfg, "agentDiscover", @[""], timeoutSec = 30))
    elif rest.len >= 2 and rest[0].toLowerAscii() == "add":
      let (card, err) = readPeerCard(rest[1 .. ^1].join(" "))
      if err != "":
        fail(err); quit(1)
      let res = daemonCall(cfg, "agentImportCard", @[card], timeoutSec = 30)
      if res.contains("\"error\""):
        fail(res); quit(1)
      ok("peer imported")
      echo formatJson(res)
    else:
      fail("Usage: pilot peer add <card.json | pasted card>   (no args lists known peers)")
      quit(1)
```

And in USAGE, under `contact`:

```
  peer add <card.json>         Learn a peer from its Agent Card (list them with: peer)
```

- [ ] **Step 6: Wire the chat commands**

In `pilot-cli/src/repl.nim` `dispatchSlash`, beside `/contact`:

```nim
  of "/peer":
    if parts.len < 3 or parts[1].toLowerAscii() != "add":
      return RED & "  usage: /peer add <card.json | pasted card>" & RESET
    let (card, perr) = readPeerCard(parts[2 .. ^1].join(" "))
    if perr != "":
      return RED & "  " & perr & RESET
    return formatJson(daemonCall(cfg, "agentImportCard", @[card], timeoutSec = 30))
  of "/peers":
    return formatJson(discoverWithRetry(cfg, "", 5))
```

And in `HELP_TEXT`, under `/contacts`:

```
  /peer add <card>             Learn a peer from its Agent Card
  /peers                       List known peers
```

- [ ] **Step 7: Build and check the help output**

Run: `cd pilot-cli && nix --extra-experimental-features 'nix-command flakes' build && ./result/bin/pilot help`
Expected: the `peer add` line appears.

- [ ] **Step 8: Commit**

```bash
git add pilot-cli/src/rpc.nim pilot-cli/pilot.nim pilot-cli/src/repl.nim pilot-cli/tests/test_rpc.nim
git commit -m "feat(cli): pilot peer add — learn a peer from its Agent Card"
```

---

### Task 5: Close the dead-drop

`a2aRoutingKeyFor` currently returns a bare hex address verbatim as if it were the peer's messaging key, so a request is encrypted and published to `/pilot/1/inbox-<that key>/proto` — a channel the peer never listens on. Passing a wallet **viewing** key (which `test-two-agents-docker.sh` does) hits exactly this. Only after Task 3/4 is refusing safe, because import is now the way to become routable.

**Files:**
- Modify: `pilot-module/src/pilot_a2a.cpp` (`a2aRoutingKeyFor`)
- Test: `pilot-module/tests/test_agent_card.cpp`

**Interfaces:**
- Consumes: `agentImportCard` (Task 3).
- Produces: no signature change.

- [ ] **Step 1: Write the failing test**

```cpp
// A bare hex string is NOT evidence of a messaging key. Routing to one encrypts
// the request to a channel nobody listens on — a silent dead-drop, which is worse
// than an error. Only a card (imported or discovered) makes a peer routable.
LOGOS_TEST(routingKey_refuses_bare_hex_without_a_card) {
    PilotImpl impl;
    impl.initialize(testDataDir());
    LOGOS_ASSERT(impl.a2aRoutingKeyForTest(
        "02a36ce18bf4221d22f28ee9ee2d5c4e7e5161fabf0995b29eb5cee1ed3e98951d").empty());
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd pilot-module && nix build .#unit-tests -L`
Expected: FAIL — the bare key is returned verbatim.

- [ ] **Step 3: Replace the fallback**

```cpp
    // No card resolved a messaging key. Previously a bare hex address was returned
    // verbatim on the theory that it might BE an ECIES key — but a wallet viewing
    // key is also bare hex, and routing to that publishes the request to a channel
    // the peer never subscribes to: a silent dead-drop (observed 2026-07-26).
    // Import or discover a card instead; callers refuse to send on empty.
    return std::string();
```

- [ ] **Step 4: Run the tests**

Run: `cd pilot-module && nix build .#unit-tests -L`
Expected: PASS, including Task 2's test, which now takes this path.

- [ ] **Step 5: Commit**

```bash
git add pilot-module/src/pilot_a2a.cpp pilot-module/tests/test_agent_card.cpp
git commit -m "fix(a2a): never route to an address no card vouches for"
```

---

### Task 6: End-to-end — prove LEZ actually moves

**Files:**
- Modify: `test-two-agents-docker.sh` (new Phase 7)

**Interfaces:**
- Consumes: `check_has()` (already present), `call_a`, `call_b`, `NPK_A`, `NPK_B`.
- Produces: a phase that fails when money does not move.

- [ ] **Step 1: Write the failing phase**

Append after Phase 6:

```bash
echo "── Phase 7: A2A Paid Task (the headline claim) ──"

# B must have a language model: agent-ask is the ONLY sellable skill.
if [ -n "${DEEPSEEK_API_KEY:-}" ]; then
  call_b metaConfigure llm.provider deepseek        > /dev/null
  call_b metaConfigure llm.api_key "$DEEPSEEK_API_KEY" > /dev/null
  call_b metaConfigure llm.model deepseek-v4-pro    > /dev/null
fi

# Hand B's card to A directly — no dependence on broadcast discovery.
CARD_B=$(call_b agentCard)
R=$(call_a agentImportCard "$CARD_B")
check_has "[A] imports B's card" "$R" '"imported":true'

BAL_A_BEFORE=$(call_a walletBalance | grep -oE '"balance":"[0-9]+"' | grep -oE '[0-9]+')
R=$(call_a agentTask "{\"agent_address\":\"$(echo "$CARD_B" | python3 -c 'import sys,json;print(json.load(sys.stdin)["_logos"]["npk"])')\",\"skill\":\"agent-ask\",\"params\":\"{\\\"prompt\\\":\\\"In one word: what colour is the sky?\\\"}\"}")
check_has "[A→B] paid task accepted" "$R" '"state":"submitted"'

echo "  ...   Waiting up to 120s for settlement"
for i in $(seq 1 12); do
  sleep 10
  STATE=$(sqlite3 /tmp/agent-a/pilot.db \
    "SELECT state FROM outbound_tasks ORDER BY rowid DESC LIMIT 1;" 2>/dev/null)
  [ "$STATE" = "paid" ] && break
  [ "$STATE" = "pay-failed" ] && break
done
check_has "[A] task reached a terminal payment state" "$STATE" '^(paid|pay-failed)$'
check_has "[A] task was PAID" "$STATE" '^paid$'

BAL_A_AFTER=$(call_a walletBalance | grep -oE '"balance":"[0-9]+"' | grep -oE '[0-9]+')
echo "  A balance ${BAL_A_BEFORE:-?} -> ${BAL_A_AFTER:-?}"
if [ -n "$BAL_A_BEFORE" ] && [ -n "$BAL_A_AFTER" ] && [ "$BAL_A_AFTER" -lt "$BAL_A_BEFORE" ]; then
  echo "  PASS  [A] balance decreased by the price"; ((PASS++))
else
  echo "  FAIL  [A] balance did not move (${BAL_A_BEFORE:-?} -> ${BAL_A_AFTER:-?})"; ((FAIL++))
fi
echo ""
```

- [ ] **Step 2: Run it and watch it fail**

Run: `DEEPSEEK_API_KEY=<key> bash test-two-agents-docker.sh`
Expected: Phase 7 fails until Tasks 2–5 are in the installed module. Record which assertion fails first.

- [ ] **Step 3: Reinstall the module and rerun**

Run: `./setup-modules.sh && DEEPSEEK_API_KEY=<key> bash test-two-agents-docker.sh`
Expected: Phase 7 passes; `A balance 100 -> 95`.

- [ ] **Step 4: Fix Phase 6's false pass while here**

`[A→B] walletSend` printed PASS over a log line reading `Transfer failed: InsufficientFundsError`. Replace with:

```bash
R=$(call_a walletSend "$NPK_B" 1 "test transfer A to B")
check_has "[A→B] walletSend settled" "$R" '"status":"(completed|held)"'
```

- [ ] **Step 5: Commit**

```bash
git add test-two-agents-docker.sh
git commit -m "test(a2a): assert LEZ actually moves on a paid task"
```

---

### Task 7: Say which state we are in

Six hours went into this diagnosis largely because a spinner, an empty result and a dead module all looked identical. Discovery retry (committed 2026-07-26) started this; finish it on the paths a demo touches.

**Files:**
- Modify: `pilot-cli/src/rpc.nim` (`explainRpcFailure` — extend), `pilot-cli/src/repl.nim`
- Test: `pilot-cli/tests/test_rpc.nim`

**Interfaces:**
- Consumes: `explainRpcFailure(resp, logTail: string): string` (existing).
- Produces: `proc syncHint*(logTail: string): string` — "" when the chain is not replaying, else `waiting: the wallet is 340 blocks behind`.

- [ ] **Step 1: Write the failing test**

```nim
# A cold agent replays the chain before it can answer. Silence during that looks
# identical to a hang; it took 37s per boot on a 3.1 GB chain (2026-07-26).
doAssert syncHint("[logos_execution_zone] Syncing to block 3269. Blocks to sync: 51")
  .contains("51")
doAssert syncHint("[logos_execution_zone] Synced to block 3269 in 1.8s") == ""
doAssert syncHint("") == ""
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd pilot-cli && nix --extra-experimental-features 'nix-command flakes' develop -c nim c -r tests/test_rpc.nim`
Expected: FAIL — `syncHint` undeclared.

- [ ] **Step 3: Implement it**

```nim
# The wallet cannot answer while it replays the chain. Surface that instead of a
# bare spinner: "still working" and "wedged" must not look the same.
proc syncHint*(logTail: string): string =
  var hint = ""
  for line in logTail.splitLines():
    let idx = line.find("Blocks to sync:")
    if idx >= 0:
      hint = "the wallet is catching up — " & line[idx + 16 .. ^1].strip() & " blocks behind"
    elif line.contains("Synced to block"):
      hint = ""
  return hint
```

- [ ] **Step 4: Show it in the chat spinner**

In `repl.nim`, where a daemon call is about to run with a spinner, read the last 20 lines of `gCfg.dataDir / "daemon.log"` and, when `syncHint` returns non-empty, append it to the spinner label.

- [ ] **Step 5: Run the tests**

Run: `cd pilot-cli && nix --extra-experimental-features 'nix-command flakes' develop -c nim c -r tests/test_rpc.nim`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add pilot-cli/src/rpc.nim pilot-cli/src/repl.nim pilot-cli/tests/test_rpc.nim
git commit -m "feat(cli): say when the wallet is replaying the chain"
```

---

### Task 8: Settle the broadcast question, then document it honestly

Independent of Tasks 1–7 and runnable at any point. It decides whether broadcast discovery is a bug we own or an upstream membership requirement.

**Files:**
- Create: `private/probe-msg-hash.sh`
- Modify: `KNOWN_LIMITATIONS.md` (new §7)

- [ ] **Step 1: Write the probe**

```bash
#!/usr/bin/env bash
# When A publishes its card its node logs the message hash. If B's node never
# logs that hash, the message is dropped in transit (RLN membership is the prime
# suspect — the module logs "Publishing message without RLN proof"). If B DOES
# log it, the transport is fine and our read path is at fault.
set -uo pipefail
LOGOSCORE=$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f | head -1)
timeout 60 $LOGOSCORE --config-dir /tmp/agent-a/.logoscore call pilot agentCard > /dev/null
sleep 3
HASH=$(grep -oE "start publish Waku message.*msg_hash=0x[0-9a-f]+" /tmp/agent-a/daemon.log \
        | tail -1 | grep -oE "0x[0-9a-f]+")
echo "A published: $HASH"
sleep 20
echo -n "B saw it: "
docker logs pilot-agent-b 2>&1 | grep -c "$HASH"
```

- [ ] **Step 2: Run it with both agents up**

Run: `bash private/probe-msg-hash.sh`
Expected: a count of 0 (dropped in transit) or ≥1 (transport fine, read path ours).

- [ ] **Step 3: Write §7 of KNOWN_LIMITATIONS.md**

State the observed result, the evidence (`Publishing message without RLN proof`, the hash count), which of the two conclusions it supports, and that out-of-band card import (Tasks 3–4) is the reliable path either way. No hedging, no "appears to".

- [ ] **Step 4: Commit**

```bash
git add private/probe-msg-hash.sh KNOWN_LIMITATIONS.md
git commit -m "docs: settle whether broadcast discovery is ours or upstream"
```

---

## Verification (the whole point)

The plan is done when this sequence, from a clean chain, prints a balance that went down:

```bash
pkill -f sequencer_service; rm -rf ~/dev/logos/logos-execution-zone/rocksdb
bash run-sequencer.sh &                       # wait for :3040 to answer 200
./setup-modules.sh
DEEPSEEK_API_KEY=<key> bash test-two-agents-docker.sh
```

Expected tail:

```
── Phase 7: A2A Paid Task (the headline claim) ──
  PASS  [A] imports B's card
  PASS  [A→B] paid task accepted
  PASS  [A] task reached a terminal payment state
  PASS  [A] task was PAID
  A balance 100 -> 95
  PASS  [A] balance decreased by the price
```

Until that block appears with real numbers, demo three is not filmable and must not be claimed.
