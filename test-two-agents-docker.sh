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

# Money-truth reader. A spend row turns COMPLETED at MEMPOOL-ACCEPT (wallet-ffi sets
# success:true on send_transaction alone), and the sequencer only folds the mempool into a
# block every BLOCK_TIME (15s here). So a walletBalance read the instant the row turns
# terminal is a read from INSIDE the block interval, and it reports the pre-block figure —
# which is chain-true for that second and wrong as a verdict. Measured 2026-08-26: the A2A
# spend completed at 17:19:46Z, the balance was read at ~17:19:47Z (99), the block carrying
# the tx was produced at 17:20:02Z, and the same wallet synced later read 94. The transfer
# was never in doubt; the reading was. Poll walletBalance (it syncs to head before
# answering) for up to four block intervals and stop the moment it moves. Prints the
# last reading. Never pass a target: a poll that waits for a specific number would be
# rewarded for guessing; this one only waits for the chain.
wait_balance_drop() {   # $1 = balance before; prints the balance after
  local before="$1" after=""
  for _ in $(seq 1 12); do
    after=$(call_a walletBalance | grep -oE '"balance":"[0-9]+"' | grep -oE '[0-9]+')
    if [ -n "$after" ] && [ -n "$before" ] && [ "$after" -lt "$before" ]; then break; fi
    sleep 5
  done
  echo "$after"
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

# Transport preflight (2026-08-18). Both 10/19 runs failed for the same reason: neither
# agent ever connected to the LOCAL relay, and the public-fleet fallback is quicksand from
# this network (A's 5 fleet conns rotted mid-run; B's container can't resolve DNS via
# 1.1.1.1 at all; hongkong-01 refuses TCP outright). The local relay was never dialed
# because neither address was dialable:
#   - A's built-in default /ip4/127.0.0.1/tcp/30303 has no /p2p/ peer id → the peer
#     manager never attempts it (zero dial attempts in a full run's log), and
#   - B got /ip4/host.docker.internal/tcp/30303 — /ip4/ demands a literal IP, so the
#     multiaddr is malformed and silently dropped.
# Fetch the relay's identity at runtime (the peer id CHANGES on every container recreate
# — no --nodekey) and hand both agents a COMPLETE multiaddr. B joins nwaku's compose
# network and dials its container IP directly: no port mapping, no DNS, no gateway guess.
NWAKU_ID=$(curl -s -m 5 http://127.0.0.1:8645/debug/v1/info | python3 -c \
  "import sys,json;print(json.load(sys.stdin)['listenAddresses'][0].rsplit('/p2p/',1)[1])" 2>/dev/null)
if [ -z "$NWAKU_ID" ]; then
  echo "  ✗ Local nwaku not answering on :8645 — start it (docker compose up -d)."
  echo "    Without it both agents fall back to the public fleet and the run scores fiction."
  exit 1
fi
NWAKU_NET=$(docker inspect pilot-nwaku -f '{{range $k,$v := .NetworkSettings.Networks}}{{$k}}{{end}}')
NWAKU_IP=$(docker inspect pilot-nwaku -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
if [ -z "$NWAKU_NET" ] || [ -z "$NWAKU_IP" ]; then
  echo "  ✗ Could not read pilot-nwaku's network/IP from docker inspect"
  exit 1
fi
export PILOT_WAKU_ADDR="/ip4/127.0.0.1/tcp/30303/p2p/$NWAKU_ID"
# The pull path (agentPoll) reads the relay's REST store directly; B reaches it by container IP.
export PILOT_WAKU_REST="http://127.0.0.1:8645"
echo "  ✓ Local relay: $NWAKU_IP (net $NWAKU_NET) p2p ${NWAKU_ID:0:16}…"
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
for m in capability_module lez_core delivery_module chat_module pilot; do
  if [ ! -d "$MODULES/$m" ]; then
    echo "  ·     $m not installed — skipping"
    continue
  fi
  cp -r $MODULES/$m $MODULES_B/$m || { echo "  ✗ could not copy $m into $MODULES_B"; exit 1; }
done

# Assert what B genuinely cannot work without actually landed. An empty /modules is not a slow
# agent, it is no agent, and every B result downstream would be fiction — so this aborts rather
# than reports. (pilot = the agent, delivery = A2A transport, LEZ = wallet for the paid task.)
for need in pilot delivery_module lez_core; do
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

# Module loads also outgrew their 15s cap: lez_core opens the wallet and can
# replay a long chain on the way up, so a short timeout silently leaves it unloaded and every
# later wallet call fails for a reason that has nothing to do with the code under test.
for m in capability_module lez_core delivery_module storage_module pilot; do
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
# ubuntu:22.04 ships with ZERO CA certificates (243 on the host, 0 in the container), so every
# HTTPS call B makes fails before it starts. Measured 2026-07-30: B accepted the paid task, then
# its agent-ask died with
#   "SSL handshake failed: The issuer certificate of a locally looked up certificate
#    could not be found"
# and it honestly replied 'failed' — so A correctly refused to pay and the paid loop could never
# close, with nothing wrong in the payment code at all.
#
# Mount ONLY the bundle file, and set SSL_CERT_FILE (not SSL_CERT_DIR). Mounting the whole
# /etc/ssl/certs directory looks tidier but is actively wrong here: 242 of its 243 entries are
# symlinks that chain into /usr/share/ca-certificates, which is NOT mounted, so inside the
# container they all dangle (measured 2026-07-30: 242 broken links). Pointing SSL_CERT_DIR at
# that is worse than having no certs, and the run where it was mounted that way is also the run
# where B stopped receiving anything at all. The bundle is the single real file, and the module
# is nix-built so SSL_CERT_FILE / NIX_SSL_CERT_FILE are what it honours.
#
# Pin the LLM API host into the container (2026-08-26). Docker's embedded DNS (127.0.0.11,
# forwarding to the host's resolvers) answered for api.deepseek.com in the 18:14 run and
# returned "Host api.deepseek.com not found" in the 19:44 run — same image, same network,
# same key. B then honestly replied 'failed' and A rightly paid nothing, so the run was
# scored on the WORK and the payment path never ran. Resolve once on the host and hand the
# container a static entry; its own DNS is left as it was for everything else.
LLM_API_HOST=api.deepseek.com
LLM_API_IP=$(getent hosts $LLM_API_HOST | awk '{print $1}' | head -1)
LLM_HOST_PIN=""
if [ -n "$LLM_API_IP" ]; then
  LLM_HOST_PIN="--add-host=$LLM_API_HOST:$LLM_API_IP"
  echo "  ·     pinned $LLM_API_HOST -> $LLM_API_IP for Agent B"
else
  echo "  WARN  host cannot resolve $LLM_API_HOST — B's agent-ask will depend on container DNS"
fi
docker run --rm -d \
  --name $CONTAINER \
  --network $NWAKU_NET \
  -v /nix/store:/nix/store:ro \
  -v $MODULES_B:/modules:ro \
  -v $AGENT_B:/data \
  -v /home/johnnie/.risc0:/root/.risc0:ro \
  -v /etc/ssl/certs/ca-certificates.crt:/etc/ssl/certs/ca-certificates.crt:ro \
  -e SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt \
  -e NIX_SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt \
  -e LOGOS_HOST_PATH=$LOGOS_HOST_PATH \
  -e PILOT_SEQUENCER_ADDR=http://host.docker.internal:3040 \
  -e PILOT_WAKU_ADDR=/ip4/$NWAKU_IP/tcp/30303/p2p/$NWAKU_ID \
  -e PILOT_WAKU_REST=http://$NWAKU_IP:8645 \
  -e RISC0_DEV_MODE=1 \
  -e LOGOS_BLOCKCHAIN_CIRCUITS=/nix/store/y8i3f2qiyhbl9kccvl7z12rnbj6h42g9-logos-blockchain-circuits-0.4.1 \
  -e PATH=/root/.risc0/extensions/v3.0.5-cargo-risczero-x86_64-unknown-linux-gnu:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  --add-host=host.docker.internal:host-gateway \
  $LLM_HOST_PIN \
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

for m in capability_module lez_core delivery_module pilot; do
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

# The module keeps working AFTER initialize returns: self-funding and the first wallet
# sync ride the same single thread, and every RPC fired into that busy window is
# abandoned by the daemon at ~40s — scored here as an empty-response FAIL over an agent
# that is merely mid-funding. Measured 2026-08-18: the same agentCard that "failed"
# mid-window answered in under a second once the module went quiet, and the relay store
# held five cards proving the runs' late publishes really went out. Gate the phases on
# the agent ANSWERING, not on initialize returning.
echo "  ...   Waiting for both agents to finish funding and answer (up to 4 min each)"
for who in A B; do
  for i in $(seq 1 24); do
    if [ "$who" = "A" ]; then Q=$(call_a getAccountId); else Q=$(call_b getAccountId); fi
    [ -n "$Q" ] && { echo "  OK    Agent $who is quiet and answering"; break; }
    sleep 10
  done
done

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

# Not scored on the call's own reply: agentOpenForHire() flips the flag, then performs the
# agent's FIRST-TIME inbox subscribes (~15s each) before returning true — on a fresh agent
# that honestly outlasts the daemon's ~40s RPC abandon, so the reply reads empty while the
# open already happened (measured 2026-08-18). The truth is asserted twice below instead:
# the state read right after, and Phase 8's "listens where its card says".
R=$(call_a agentOpenForHire); echo "  ·     [A] agentOpenForHire replied: ${R:-<abandoned at daemon timeout — verified by the reads below>}"
R=$(call_b agentOpenForHire); echo "  ·     [B] agentOpenForHire replied: ${R:-<abandoned at daemon timeout — verified by the reads below>}"

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

# 30s, not 10: the card publish can complete AFTER the agentCard RPC is abandoned
# (the module finishes the send late — relay store proved it). Give the late publish
# room to land before B looks, and give B a second look for good measure.
echo "  ...   Waiting 30s for card to propagate"
sleep 30

# Discovery must FIND something. "" is the shared discovery topic agentCard()
# publishes to; passing a name builds /pilot/1/discovery-<name>/proto, which no
# card is ever published to.
R=$(call_b agentDiscover "")
if ! echo "$R" | grep -qE '"count":[1-9]'; then
  echo "  ...   First look found nothing; one more look in 20s"
  sleep 20
  R=$(call_b agentDiscover "")
fi
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
#
# And assert it from the SPEND ROW, not the RPC response: the daemon abandons a
# module RPC at ~40s (measured RPC_FAILED at 43s, 2026-07-29) while an on-chain
# transfer takes longer, so the response here is legitimately empty on the very
# runs where the transfer goes through. The module keeps working and records the
# outcome in spend_requests; poll that.
#
# HONESTY CEILING (measured 2026-07-30): COMPLETED means the wallet SUBMITTED the
# tx and the sequencer took it into its mempool — wallet-ffi sets success:true on
# send_transaction() alone. Run 01:54 recorded COMPLETED + tx_hash 490fb031… and
# the sequencer then REJECTED that tx at block production ("Nullifier already
# seen", seq.log 01:15:48Z — a LEZ bug fixed upstream in #268; the pinned rev
# carries the fix). So the row proves the transfer path ran to a terminal state;
# the balance assertion below is what measures money.
BAL6_BEFORE=$(call_a walletBalance | grep -oE '"balance":"[0-9]+"' | grep -oE '[0-9]+')
R=$(call_a walletSend "$NPK_B" 1 "test transfer A to B")
SPEND_STATE=""
for _ in $(seq 1 24); do
  SPEND_STATE=$(rm -rf /tmp/spendchk && mkdir -p /tmp/spendchk && \
    cp $AGENT_A/pilot.db /tmp/spendchk/ 2>/dev/null && \
    cp $AGENT_A/pilot.db-wal /tmp/spendchk/ 2>/dev/null; \
    python3 -c "
import sqlite3
try:
    con = sqlite3.connect('/tmp/spendchk/pilot.db')
    rows = list(con.execute(\"select state from spend_requests where reason='test transfer A to B' order by rowid desc limit 1\"))
    print(rows[0][0] if rows else '')
except Exception:
    print('')" 2>/dev/null)
  case "$SPEND_STATE" in
    COMPLETED|TX_FAILED|TX_UNKNOWN|HELD|NOTIFIED|REJECTED|EXPIRED) break ;;
  esac
  sleep 5
done
check_has "[A→B] walletSend spend reached a terminal state" "state:$SPEND_STATE" 'state:(COMPLETED|HELD|NOTIFIED)'

# The same pay-by-keys route Phase 7 settles on (walletSend with B's keys JSON ->
# transfer_private), asserted on the chain, not the row. A HELD/NOTIFIED spend is
# parked for the owner and moves nothing — say so instead of failing a balance
# that was never supposed to change. Also pins BAL_A_BEFORE in Phase 7 to a
# settled figure: without this wait, a Phase 6 tx still in the mempool would
# land during Phase 7 and show up there as a 6-LEZ drop on a 5-LEZ price.
if [ "$SPEND_STATE" = "COMPLETED" ]; then
  BAL6_AFTER=$(wait_balance_drop "$BAL6_BEFORE")
  echo "  A balance ${BAL6_BEFORE:-?} -> ${BAL6_AFTER:-?} (walletSend 1 LEZ)"
  if [ -n "$BAL6_BEFORE" ] && [ -n "$BAL6_AFTER" ] && [ $((BAL6_BEFORE - BAL6_AFTER)) -eq 1 ]; then
    echo "  PASS  [A→B] balance decreased by exactly 1 LEZ"; ((PASS++))
  else
    echo "  FAIL  [A→B] balance did not decrease by 1 LEZ (${BAL6_BEFORE:-?} -> ${BAL6_AFTER:-?})"; ((FAIL++))
  fi
else
  echo "  ·     [A→B] spend is $SPEND_STATE (owner-gated) — no on-chain movement expected"
fi

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
  # Pull path (2026-08-25): push events never reach the module, so each side has to ASK the
  # relay store — B for the task in its inbox, A for B's reply on the task's reply topic.
  call_b agentPoll > /dev/null 2>&1
  call_a agentPoll > /dev/null 2>&1
  STATE=$(task_state "$AGENT_A/pilot.db" "$TASK_ID_A")
  case "$STATE" in
    paid|pay-failed|pay-unresolved) break ;;
    UNREADABLE*) break ;;
  esac
done
echo "  ...   Final task state: ${STATE:-<none>}"

# Every state settleOutboundReply can write is "readable" — including the doer's
# terminal negatives (failed/canceled/rejected: B said the WORK failed, so A
# refused to pay, which is correct and must not read as a broken ledger) and the
# owner gate (awaiting-approval / accepted-nopay). Run 19:44 on 2026-08-26 scored
# an honest 'failed' as three failures because this list stopped at 'settling'.
check_has "[A] task ledger is readable" "$STATE" \
  '^(paid|pay-failed|pay-unresolved|submitted|settling|awaiting-approval|accepted-nopay|failed|canceled|rejected|NO-SUCH-TASK)$'
check_has "[A] task reached a terminal payment state" "$STATE" '^(paid|pay-failed)$'
check_has "[A] task was PAID" "$STATE" '^paid$'

# This is the money-truth assertion: 'paid' above is written when the wallet
# reports success, and wallet-ffi reports success at MEMPOOL-ACCEPT — so 'paid'
# alone can never distinguish a settled tx from one the sequencer later drops.
# Only the chain can, and this is the line that asks it.
#
# History, so nobody re-derives it: 2026-08-01 this line was RED for a real
# reason — LEZ rejected pay-by-keys (transfer_private, the A2A route) with
# "Nullifier already seen" whenever the recipient had on-chain history. That was
# fixed upstream (#268) and the pinned lez_core rev carries the fix; pay-by-keys
# to a recipient WITH history measured 98->96 on 2026-08-16. On 2026-08-26 the
# line went RED again (99 -> 99) with the tx in a 2-tx block 16s later and the
# wallet reading 94 once synced past it: the balance had been read INSIDE the
# 15s block interval. wait_balance_drop is the fix for that; the exact-price
# check below is what makes this line sharper than "it moved". Do NOT soften
# it to make the suite green — it is the only line that measures money.
PRICE_A=$(python3 - "$AGENT_A/pilot.db" "$TASK_ID_A" <<'PY' 2>/dev/null
import sqlite3, sys
db, task = sys.argv[1], sys.argv[2]
try:
    con = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    row = con.execute("SELECT price FROM outbound_tasks WHERE id=?;", (task,)).fetchone()
    print(row[0] if row and row[0] is not None else "")
except Exception:
    print("")
PY
)
BAL_A_AFTER=$(wait_balance_drop "$BAL_A_BEFORE")
echo "  A balance ${BAL_A_BEFORE:-?} -> ${BAL_A_AFTER:-?} (declared price ${PRICE_A:-?} LEZ)"
if [ -n "$BAL_A_BEFORE" ] && [ -n "$BAL_A_AFTER" ] && [ -n "$PRICE_A" ] && [ "$PRICE_A" -gt 0 ] \
   && [ $((BAL_A_BEFORE - BAL_A_AFTER)) -eq "$PRICE_A" ]; then
  echo "  PASS  [A] balance decreased by exactly the declared price"; ((PASS++))
else
  echo "  FAIL  [A] balance did not decrease by the declared price (${BAL_A_BEFORE:-?} -> ${BAL_A_AFTER:-?}, price ${PRICE_A:-?})"; ((FAIL++))
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
