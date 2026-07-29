#!/bin/bash
set -o pipefail

echo "╔══════════════════════════════════════════════╗"
echo "║   PILOT TWO-AGENT TEST (Host + Docker)      ║"
echo "╚══════════════════════════════════════════════╝"
echo ""

LOGOSCORE=$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f 2>/dev/null | head -1)
export LOGOS_HOST_PATH=$(find /nix/store -maxdepth 3 -name logos_host -path "*liblogos-bin*" -type f 2>/dev/null | head -1)

# Agent B's container gets RISC0_DEV_MODE=1 + circuits via docker -e flags; the
# HOST Agent A only inherits the launching shell. A bare shell means the wallet
# side proves for real / can't find its circuits and the pilot module dies or
# wedges on initialize — every A call then reads "(empty response)" while B is
# fine (seen live 2026-07-11). Bake the env like pilot-cli's .start-daemon.sh.
export RISC0_DEV_MODE="${RISC0_DEV_MODE:-1}"
if [ -z "${LOGOS_BLOCKCHAIN_CIRCUITS:-}" ]; then
  export LOGOS_BLOCKCHAIN_CIRCUITS=$(find /nix/store -maxdepth 1 -name '*logos-blockchain-circuits*' -type d 2>/dev/null | head -1)
fi
[ -d "$HOME/.risc0/extensions/v3.0.5-cargo-risczero-x86_64-unknown-linux-gnu" ] && \
  export PATH="$HOME/.risc0/extensions/v3.0.5-cargo-risczero-x86_64-unknown-linux-gnu:$PATH"
MODULES="${PILOT_MODULE_PATH:-$HOME/.pilot/modules}"
MODULES_B="/tmp/pilot-logoscore/modules-b"   # scratch copy for Agent B (repopulated from $MODULES below)

AGENT_A="/tmp/agent-a"
AGENT_B="/tmp/agent-b"
CFG_A="--config-dir $AGENT_A/.logoscore"
CONTAINER="pilot-agent-b"

PASS=0
FAIL=0

check() {
  local label="$1" result="$2"
  if echo "$result" | grep -q '"error"'; then
    echo "  FAIL  $label"
    echo "        → $(echo "$result" | head -c 120)"
    ((FAIL++))
  elif [ -z "$result" ]; then
    echo "  FAIL  $label (empty response)"
    ((FAIL++))
  else
    echo "  PASS  $label"
    ((PASS++))
  fi
}

# check() only asserts "the call came back and didn't say error" — which a response
# reporting failure in plain prose still satisfies. Discovery returning
# {"agents":[],"count":0,"note":"no agents found"} passed for the life of this
# script while finding nobody, which is how a broken A2A path stayed green
# (2026-07-26). Use this when the RESULT, not the call, is what matters.
check_has() {
  local label="$1" result="$2" pattern="$3"
  if echo "$result" | grep -qE "$pattern"; then
    echo "  PASS  $label"
    ((PASS++))
  else
    echo "  FAIL  $label (expected /$pattern/)"
    echo "        → $(echo "$result" | head -c 160)"
    ((FAIL++))
  fi
}

call_a() {
  # 120s to match call_b: a cold agent replays the whole chain inside initialize,
  # and 30s turned a long chain into bogus "(empty response)" failures.
  # 300s, not 120s. The caller must outlast the agent, never race it: agentTask arms a reply
  # topic on a delivery module that is busy relaying a live public network, and that single
  # call can itself take up to 120s. With both set to 120 the caller gave up while the agent
  # was still working — the task really was submitted and B really did reply (6 messages
  # landed on the reply topic), but the caller saw an empty response and the run scored it a
  # failure. A timeout that expires mid-success reports fiction.
  local raw=$(timeout 300 $LOGOSCORE $CFG_A call pilot "$@" 2>&1)
  echo "$raw" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('result',''))" 2>/dev/null || echo "$raw"
}

call_b() {
  local raw=$(docker exec $CONTAINER timeout 300 $LOGOSCORE --config-dir /data/.logoscore call pilot "$@" 2>&1)
  echo "$raw" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('result',''))" 2>/dev/null || echo "$raw"
}

# Pre-flight
if ! curl -s -o /dev/null -w "" http://127.0.0.1:3040/ 2>/dev/null; then
  echo "  ✗ Sequencer not running on port 3040 (start it: ./run-sequencer.sh)"
  exit 1
fi
echo "  ✓ Sequencer reachable"

if [ ! -d "$MODULES" ] || [ -z "$(ls -A $MODULES 2>/dev/null)" ]; then
  echo "  ✗ No modules in $MODULES"
  exit 1
fi
echo "  ✓ Modules installed"
echo ""

# Clean
pkill -9 -f logos_host_qt 2>/dev/null
pkill -9 -f logoscore 2>/dev/null
docker stop $CONTAINER 2>/dev/null
rm -rf $AGENT_A ~/.cache/storage/dht/providers/LOCK
docker run --rm -v /tmp:/tmp ubuntu:22.04 rm -rf /tmp/agent-b 2>/dev/null
mkdir -p $AGENT_A $AGENT_B

# Prepare modules-b (no storage_module).
#
# Docker creates a missing bind-mount source as ROOT, so $MODULES_B and its parent come back
# root-owned after any run — and then `rm -rf`/`mkdir -p`/`cp` all fail for this user. Every
# one of those failures was silenced (2>/dev/null), so the run continued with an EMPTY
# /modules mounted into the container and printed "Agent B modules loaded" over an agent that
# had loaded nothing at all. Observed 2026-07-29: 26/31 became a meaningless run against a
# phantom peer. Clear the leftovers as root first — the same trick this script already uses
# for /tmp/agent-b one line above.
docker run --rm -v /tmp:/tmp ubuntu:22.04 rm -rf $MODULES_B 2>/dev/null
rm -rf $MODULES_B 2>/dev/null
mkdir -p $MODULES_B || { echo "  ✗ cannot create $MODULES_B"; exit 1; }
# capability_module is listed but is not installed on this box, and B has never needed it —
# so a missing OPTIONAL module is noted and skipped, not fatal.
for m in capability_module logos_execution_zone delivery_module chat_module pilot; do
  if [ ! -d "$MODULES/$m" ]; then
    echo "  ·     $m not installed — skipping"
    continue
  fi
  cp -r $MODULES/$m $MODULES_B/$m || { echo "  ✗ could not copy $m into $MODULES_B"; exit 1; }
done

# Assert what B genuinely cannot work without actually landed. An empty /modules is not a slow
# agent, it is no agent, and every B result downstream would be fiction — so this aborts rather
# than reports. (pilot = the agent, delivery = A2A transport, LEZ = wallet for the paid task.)
for need in pilot delivery_module logos_execution_zone; do
  if [ ! -d "$MODULES_B/$need" ]; then
    echo "  ✗ Agent B is missing $need — refusing to run a phantom peer"
    ls -la $MODULES_B
    exit 1
  fi
done
echo "  ✓ Agent B modules staged ($(ls -1 $MODULES_B | wc -l): $(ls -1 $MODULES_B | tr '\n' ' '))"
sleep 2

# ═══════════════════════════════════════
echo "── Starting Agent A (host) ──"
setsid $LOGOSCORE $CFG_A -D -m $MODULES > $AGENT_A/daemon.log 2>&1 &

# Wait for the daemon to actually be up, don't guess at how long it takes. A flat `sleep 5`
# failed this run on a loaded box: the daemon was still loading its first module at the 5s
# mark, the single check missed it, the test aborted — and the daemon then came up 3 seconds
# later, orphaned, with nothing to reap it. Poll the condition instead.
for i in $(seq 1 40); do
  grep -q '"pid"' "$AGENT_A/.logoscore/daemon/state.json" 2>/dev/null && break
  sleep 2
done
if ! grep -q '"pid"' "$AGENT_A/.logoscore/daemon/state.json" 2>/dev/null; then
  echo "  FAIL  Agent A daemon did not start within 80s"
  tail -15 "$AGENT_A/daemon.log" 2>/dev/null
  pkill -9 -f logos_host_qt 2>/dev/null
  exit 1
fi
echo "  OK    Agent A daemon running"

# Module loads also outgrew their 15s cap: logos_execution_zone opens the wallet and can
# replay a long chain on the way up, so a short timeout silently leaves it unloaded and every
# later wallet call fails for a reason that has nothing to do with the code under test.
for m in capability_module logos_execution_zone delivery_module storage_module pilot; do
  timeout 120 $LOGOSCORE $CFG_A load-module $m > /dev/null 2>&1
done
sleep 3
echo "  OK    Agent A modules loaded"

# ═══════════════════════════════════════
echo ""
echo "── Starting Agent B (Docker) ──"
# A stale pilot-agent-b (e.g. --rm never fired because Docker Desktop shut down
# uncleanly) holds the name and makes docker run fail silently into /dev/null;
# the status check then reports "did not start" while docker logs shows the OLD
# container happily syncing — remove any corpse first.
docker rm -f $CONTAINER >/dev/null 2>&1
docker run --rm -d \
  --name $CONTAINER \
  -v /nix/store:/nix/store:ro \
  -v $MODULES_B:/modules:ro \
  -v $AGENT_B:/data \
  -v /home/johnnie/.risc0:/root/.risc0:ro \
  -e LOGOS_HOST_PATH=$LOGOS_HOST_PATH \
  -e PILOT_SEQUENCER_ADDR=http://host.docker.internal:3040 \
  -e PILOT_WAKU_ADDR=/ip4/host.docker.internal/tcp/30303 \
  -e RISC0_DEV_MODE=1 \
  -e LOGOS_BLOCKCHAIN_CIRCUITS=/nix/store/y8i3f2qiyhbl9kccvl7z12rnbj6h42g9-logos-blockchain-circuits-0.4.1 \
  -e PATH=/root/.risc0/extensions/v3.0.5-cargo-risczero-x86_64-unknown-linux-gnu:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  --add-host=host.docker.internal:host-gateway \
  ubuntu:22.04 \
  $LOGOSCORE --config-dir /data/.logoscore -D -m /modules \
  > /dev/null 2>&1

sleep 5
if ! docker ps --filter name=$CONTAINER --format "{{.Status}}" | grep -q "Up"; then
  echo "  FAIL  Agent B container did not start"
  docker logs $CONTAINER 2>&1 | tail -5
  exit 1
fi
echo "  OK    Agent B container running"

for m in capability_module logos_execution_zone delivery_module pilot; do
  docker exec $CONTAINER timeout 15 $LOGOSCORE --config-dir /data/.logoscore load-module $m > /dev/null 2>&1
done
sleep 3
echo "  OK    Agent B modules loaded"
echo ""

# ═══════════════════════════════════════
echo "── Phase 1: Initialize Both Agents ──"

R=$(call_a initialize $AGENT_A)
if [ -z "$R" ]; then   # same slow-cold-boot tolerance as Agent B below
  for i in $(seq 1 18); do
    sleep 10
    R=$(call_a getAccountId)
    [ -n "$R" ] && break
  done
fi
check "[A] initialize" "$R"

R=$(call_b initialize /data)
# A cold Agent B self-funds AND replays the whole chain inside initialize — on a
# loaded box (sequencer + host daemon + Docker) that can outlast one call window
# and reply empty while B is actually fine. Poll until the identity answers
# instead of failing the run (and cascading NPK_B="" into bogus
# "invalid recipient key" failures downstream).
if [ -z "$R" ]; then
  for i in $(seq 1 18); do
    sleep 10
    R=$(call_b getAccountId)
    [ -n "$R" ] && break
  done
fi
check "[B] initialize" "$R"

NPK_A=$(call_a getAgentNpk)
check "[A] getAgentNpk" "$NPK_A"

NPK_B=$(call_b getAgentNpk)
for i in $(seq 1 6); do
  [ -n "$NPK_B" ] && break
  sleep 10
  NPK_B=$(call_b getAgentNpk)
done
check "[B] getAgentNpk" "$NPK_B"

ACCOUNT_A=$(call_a getAccountId)
ACCOUNT_B=$(call_b getAccountId)

VIEW_A=$(echo "$NPK_A" | python3 -c "import sys,json; print(json.load(sys.stdin).get('viewing_public_key',''))" 2>/dev/null)
VIEW_B=$(echo "$NPK_B" | python3 -c "import sys,json; print(json.load(sys.stdin).get('viewing_public_key',''))" 2>/dev/null)

echo ""
echo "  Agent A: $(echo $ACCOUNT_A | head -c 24)..."
echo "  Agent B: $(echo $ACCOUNT_B | head -c 24)..."
echo ""

# ═══════════════════════════════════════
echo "── Phase 1b: Open Both Agents For Hire ──"

# An agent does NOT put itself up for sale on boot — taking work from strangers is the
# owner's explicit decision. So a fresh agent is CLOSED: no inbox subscription, and
# agentCard() builds a card but does not broadcast it. Everything downstream (discovery in
# Phase 2, the paid task in Phase 7) depends on this step, which is exactly why it is
# asserted rather than assumed.
# call_a/call_b render the result through python, so a JSON boolean comes back as Python's
# True/False — capitalised. Matching lowercase only reported six failures over agents that had
# in fact opened correctly (Phase 8 could not otherwise pass). Match either spelling.
for who in A B; do
  if [ "$who" = "A" ]; then WAS=$(call_a agentIsOpenForHire); else WAS=$(call_b agentIsOpenForHire); fi
  check_has "[$who] starts CLOSED for hire" "$WAS" '^([Ff]alse|0|)$'
done

R=$(call_a agentOpenForHire); check_has "[A] opens for hire" "$R" '^([Tt]rue|1)$'
R=$(call_b agentOpenForHire); check_has "[B] opens for hire" "$R" '^([Tt]rue|1)$'

for who in A B; do
  if [ "$who" = "A" ]; then NOW=$(call_a agentIsOpenForHire); else NOW=$(call_b agentIsOpenForHire); fi
  check_has "[$who] now reports OPEN for hire" "$NOW" '^([Tt]rue|1)$'
done
echo ""

# ═══════════════════════════════════════
echo "── Warming up delivery (Agent B) ──"
call_b establishOwnerChannel > /dev/null 2>&1
echo "  OK    Delivery initialized"
echo ""

# ═══════════════════════════════════════
echo "── Phase 2: Agent Discovery ──"

R=$(call_a agentCard)
check "[A] publishes Agent Card" "$R"

echo "  ...   Waiting 10s for card to propagate"
sleep 10

# Discovery must FIND something. "" is the shared discovery topic agentCard()
# publishes to; passing a name builds /pilot/1/discovery-<name>/proto, which no
# card is ever published to.
R=$(call_b agentDiscover "")
check_has "[B] discovers ≥1 agent" "$R" '"count":[1-9]'
check_has "[B] discovered agent is A" "$R" "$(echo "$NPK_A" | python3 -c "import sys,json;print(json.load(sys.stdin).get('nullifier_public_key','__none__')[:16])" 2>/dev/null || echo '__none__')"

echo ""

# ═══════════════════════════════════════
echo "── Phase 3: Cross-Agent Messaging ──"

R=$(call_a messagingSend "$VIEW_B" "Hello from Agent A")
check "[A→B] messagingSend" "$R"

R=$(call_b messagingSend "$VIEW_A" "Hello from Agent B")
check "[B→A] messagingSend" "$R"

echo ""

# ═══════════════════════════════════════
echo "── Phase 4: Storage Share ──"

echo "shared-secret-data-from-agent-a" > /tmp/agent-a-file.txt
R=$(call_a storageUpload /tmp/agent-a-file.txt "shared-doc")
check "[A] uploads file" "$R"

CID=$(echo "$R" | python3 -c "import sys,json; print(json.load(sys.stdin).get('cid',''))" 2>/dev/null)
if [ -n "$CID" ]; then
  R=$(call_a storageShare "$CID" "$VIEW_B")
  check "[A→B] shares file key" "$R"
else
  echo "  SKIP  [A→B] share (no CID from upload)"
fi

echo ""

# ═══════════════════════════════════════
echo "── Phase 5: A2A Task Lifecycle ──"

R=$(call_b agentTask "$VIEW_A" "echo" '{"input":"cross-agent-docker"}')
check "[B→A] sends task" "$R"

TASK_ID=$(echo "$R" | python3 -c "import sys,json; print(json.load(sys.stdin).get('id',''))" 2>/dev/null)

if [ -n "$TASK_ID" ]; then
  R=$(call_b agentSubscribe "$VIEW_A" "$TASK_ID")
  check "[B] subscribes to task" "$R"

  R=$(call_b agentCancel "$VIEW_A" "$TASK_ID")
  check "[B] cancels task" "$R"
else
  echo "  SKIP  subscribe/cancel (no task ID)"
fi

echo ""

# ═══════════════════════════════════════
echo "── Phase 6: Wallet Transfer ──"

# Cross-agent shielded pay needs B's public keys (npk+vpk), not its account id.
# check() passed this for the life of the script while the log underneath read
# "Transfer failed: InsufficientFundsError" — a response that reports failure in
# prose still satisfies "came back and didn't say error". Assert the SETTLEMENT.
R=$(call_a walletSend "$NPK_B" 1 "test transfer A to B")
check_has "[A→B] walletSend settled" "$R" '"status":"(completed|held)"'

echo ""

# ═══════════════════════════════════════
echo "── Phase 7: A2A Paid Task (the headline claim) ──"

# B must have a language model: agent-ask is the ONLY sellable skill.
if [ -n "${DEEPSEEK_API_KEY:-}" ]; then
  call_b metaConfigure llm.provider deepseek           > /dev/null
  call_b metaConfigure llm.api_key "$DEEPSEEK_API_KEY" > /dev/null
  call_b metaConfigure llm.model deepseek-v4-pro       > /dev/null
  echo "  OK    Agent B has a language model"
else
  echo "  WARN  DEEPSEEK_API_KEY is unset — B cannot answer agent-ask, so the"
  echo "        paid task below will fail on the WORK, not on the payment path."
fi

# Hand B's card to A directly — no dependence on broadcast discovery.
CARD_B=$(call_b agentCard)
R=$(call_a agentImportCard "$CARD_B")
check_has "[A] imports B's card" "$R" '"imported":true'
check_has "[A] B's card verifies" "$R" '"signature_status":"valid"'

NPK_B_CARD=$(echo "$CARD_B" | python3 -c \
  'import sys,json;print(json.load(sys.stdin)["_logos"]["npk"])' 2>/dev/null)
if [ -z "$NPK_B_CARD" ]; then
  echo "  FAIL  [A→B] paid task (no _logos.npk in B's card — nothing to address)"
  ((FAIL++))
fi

BAL_A_BEFORE=$(call_a walletBalance | grep -oE '"balance":"[0-9]+"' | grep -oE '[0-9]+')

R=$(call_a agentTask "$NPK_B_CARD" "agent-ask" '{"prompt":"In one word: what colour is the sky?"}')
check_has "[A→B] paid task accepted" "$R" '"state":"submitted"'
# The price is read from the IMPORTED card. "none-declared" here means the card
# never made it into the store, which would otherwise only surface much later as
# a payment that silently never happens.
check_has "[A] task carries B's declared price" "$R" '"payment":"pending-on-acceptance"'

TASK_ID_A=$(echo "$R" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("id",""))' 2>/dev/null)

# Read the state of THIS task, by id. python3 (not the sqlite3 CLI, which is not
# on this box) so a missing binary cannot return "" and read as "never paid".
# Prints a bare state, or UNREADABLE: <why> — the two must never look alike.
task_state() {
  python3 - "$1" "$2" <<'PY' 2>&1
import sqlite3, sys
db, task = sys.argv[1], sys.argv[2]
try:
    con = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    row = con.execute("SELECT state FROM outbound_tasks WHERE id=?;", (task,)).fetchone()
    print(row[0] if row else "NO-SUCH-TASK")
except Exception as e:
    print("UNREADABLE: %s" % e)
PY
}

echo "  ...   Waiting up to 120s for settlement"
STATE=""
for i in $(seq 1 12); do
  sleep 10
  STATE=$(task_state "$AGENT_A/pilot.db" "$TASK_ID_A")
  case "$STATE" in
    paid|pay-failed|pay-unresolved) break ;;
    UNREADABLE*) break ;;
  esac
done
echo "  ...   Final task state: ${STATE:-<none>}"

check_has "[A] task ledger is readable" "$STATE" '^(paid|pay-failed|pay-unresolved|submitted|settling|NO-SUCH-TASK)$'
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

# ═══════════════════════════════════════
echo "── Phase 8: Agents listen where their cards say ──"

# Every unit test in the module asserts the topic LIST. This asserts what the agent is
# ACTUALLY listening on — the topics delivery_module confirmed. The gap between those two is
# where the unhireable-agent bug lived: the card went out, the inbox was never subscribed, and
# nothing anywhere noticed.
#
# NOT a log grep. delivery_module logs a `send` with its content topic but logs NOTHING for a
# `subscribe` — measured 2026-07-27 by subscribing a canary topic directly and finding it
# nowhere in a 600KB log. A log-scraping version of this phase fails no matter how correct the
# code is, which is a false RED and every bit as useless as the false green it replaced.
for who in A B; do
  if [ "$who" = "A" ]; then
    CARD=$(call_a agentCard); SUBS=$(call_a subscribedTopics)
  else
    CARD=$(call_b agentCard); SUBS=$(call_b subscribedTopics)
  fi
  ENC=$(echo "$CARD" | python3 -c \
    'import sys,json;print(json.load(sys.stdin)["_logos"].get("enc_key",""))' 2>/dev/null)
  if [ -z "$ENC" ]; then
    echo "  FAIL  [$who] card has no _logos.enc_key to check"; ((FAIL++)); continue
  fi
  if echo "$SUBS" | grep -qF "/pilot/1/inbox-$ENC/proto"; then
    echo "  PASS  [$who] listens on the inbox its card advertises"; ((PASS++))
  else
    echo "  FAIL  [$who] advertises /pilot/1/inbox-${ENC:0:16}…/proto but is not listening there"
    echo "        → actually listening on: $(echo "$SUBS" | head -c 200)"
    ((FAIL++))
  fi
done
echo ""

# ═══════════════════════════════════════
echo "══════════════════════════════════════"
echo "  Results: $PASS passed, $FAIL failed"
echo "══════════════════════════════════════"

# Cleanup. Save B's log FIRST: the container runs with --rm, so stopping it
# deletes it and `docker logs pilot-agent-b` — which this script used to print as
# the way to read B's side — fails with "No such container". Every post-mortem of
# a failed run needs that log, and it was being thrown away.
docker logs $CONTAINER > $AGENT_B/daemon.log 2>&1
timeout 5 $LOGOSCORE $CFG_A stop > /dev/null 2>&1
docker stop $CONTAINER > /dev/null 2>&1
pkill -9 -f logos_host_qt 2>/dev/null
rm -f ~/.cache/storage/dht/providers/LOCK

echo ""
echo "  Agent A log: $AGENT_A/daemon.log"
echo "  Agent B log: $AGENT_B/daemon.log  (saved — the container is gone with --rm)"

exit $FAIL
