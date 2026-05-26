#!/bin/bash
set -o pipefail

echo "╔══════════════════════════════════════════════╗"
echo "║   PILOT TWO-AGENT TEST (Host + Docker)      ║"
echo "╚══════════════════════════════════════════════╝"
echo ""

LOGOSCORE=$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f 2>/dev/null | head -1)
export LOGOS_HOST_PATH=$(find /nix/store -maxdepth 3 -name logos_host -path "*liblogos-bin*" -type f 2>/dev/null | head -1)
MODULES="/tmp/pilot-logoscore/modules"
MODULES_B="/tmp/pilot-logoscore/modules-b"

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

call_a() {
  local raw=$(timeout 30 $LOGOSCORE $CFG_A call pilot "$@" 2>&1)
  echo "$raw" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('result',''))" 2>/dev/null || echo "$raw"
}

call_b() {
  local raw=$(docker exec $CONTAINER timeout 60 $LOGOSCORE --config-dir /data/.logoscore call pilot "$@" 2>&1)
  echo "$raw" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('result',''))" 2>/dev/null || echo "$raw"
}

# Pre-flight
if ! curl -s -o /dev/null -w "" http://127.0.0.1:8080/ 2>/dev/null; then
  echo "  ✗ Sequencer not running on port 8080"
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

# Prepare modules-b (no storage_module)
rm -rf $MODULES_B
mkdir -p $MODULES_B
for m in capability_module lez_wallet_module delivery_module chat_module pilot; do
  cp -r $MODULES/$m $MODULES_B/$m 2>/dev/null
done
sleep 2

# ═══════════════════════════════════════
echo "── Starting Agent A (host) ──"
setsid $LOGOSCORE $CFG_A -D -m $MODULES > $AGENT_A/daemon.log 2>&1 &
sleep 5

if ! cat $AGENT_A/.logoscore/daemon/state.json 2>/dev/null | grep -q '"pid"'; then
  echo "  FAIL  Agent A daemon did not start"
  exit 1
fi
echo "  OK    Agent A daemon running"

for m in capability_module lez_wallet_module delivery_module storage_module pilot; do
  timeout 15 $LOGOSCORE $CFG_A load-module $m > /dev/null 2>&1
done
sleep 3
echo "  OK    Agent A modules loaded"

# ═══════════════════════════════════════
echo ""
echo "── Starting Agent B (Docker) ──"
docker run --rm -d \
  --name $CONTAINER \
  -v /nix/store:/nix/store:ro \
  -v $MODULES_B:/modules:ro \
  -v $AGENT_B:/data \
  -e LOGOS_HOST_PATH=$LOGOS_HOST_PATH \
  -e PILOT_SEQUENCER_ADDR=http://host.docker.internal:8080 \
  -e PILOT_WAKU_ADDR=/ip4/host.docker.internal/tcp/30303 \
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

for m in capability_module lez_wallet_module delivery_module pilot; do
  docker exec $CONTAINER timeout 15 $LOGOSCORE --config-dir /data/.logoscore load-module $m > /dev/null 2>&1
done
sleep 3
echo "  OK    Agent B modules loaded"
echo ""

# ═══════════════════════════════════════
echo "── Phase 1: Initialize Both Agents ──"

R=$(call_a initialize $AGENT_A)
check "[A] initialize" "$R"

R=$(call_b initialize /data)
check "[B] initialize" "$R"

NPK_A=$(call_a getAgentNpk)
check "[A] getAgentNpk" "$NPK_A"

NPK_B=$(call_b getAgentNpk)
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

R=$(call_b agentDiscover pilot)
check "[B] discovers agents" "$R"

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

R=$(call_a walletSend "$ACCOUNT_B" 1 "test transfer A to B")
check "[A→B] walletSend" "$R"

echo ""

# ═══════════════════════════════════════
echo "══════════════════════════════════════"
echo "  Results: $PASS passed, $FAIL failed"
echo "══════════════════════════════════════"

# Cleanup
timeout 5 $LOGOSCORE $CFG_A stop > /dev/null 2>&1
docker stop $CONTAINER > /dev/null 2>&1
pkill -9 -f logos_host_qt 2>/dev/null
rm -f ~/.cache/storage/dht/providers/LOCK

echo ""
echo "  Agent A log: $AGENT_A/daemon.log"
echo "  Agent B log: docker logs $CONTAINER"

exit $FAIL
