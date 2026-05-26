#!/bin/bash
set -o pipefail

echo "╔══════════════════════════════════════════════╗"
echo "║      PILOT TWO-AGENT INTEGRATION TEST       ║"
echo "╚══════════════════════════════════════════════╝"
echo ""

LOGOSCORE=$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f 2>/dev/null | head -1)
export LOGOS_HOST_PATH=$(find /nix/store -maxdepth 3 -name logos_host -path "*liblogos-bin*" -type f 2>/dev/null | head -1)
MODULES="/tmp/pilot-logoscore/modules"

AGENT_A="/tmp/agent-a"
AGENT_B="/tmp/agent-b"
CFG_A="--config-dir $AGENT_A/.logoscore"
CFG_B="--config-dir $AGENT_B/.logoscore"

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
  local raw=$(timeout 120 $LOGOSCORE $CFG_B call pilot "$@" 2>&1)
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
rm -rf $AGENT_A $AGENT_B ~/.cache/storage/dht/providers/LOCK
mkdir -p $AGENT_A $AGENT_B
sleep 2

# ═══════════════════════════════════════
echo "── Starting Agent A ──"
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
echo "── Starting Agent B ──"

# Storage module uses a global LevelDB cache at ~/.cache/storage/ —
# two instances can't coexist on the same machine.
# Create a separate modules dir for B without storage_module.
MODULES_B="/tmp/pilot-logoscore/modules-b"
rm -rf $MODULES_B
mkdir -p $MODULES_B
for m in capability_module lez_wallet_module delivery_module chat_module pilot; do
  cp -r $MODULES/$m $MODULES_B/$m 2>/dev/null
done

PILOT_DISC_PORT=9001 setsid $LOGOSCORE $CFG_B -D -m $MODULES_B > $AGENT_B/daemon.log 2>&1 &
sleep 5

if ! cat $AGENT_B/.logoscore/daemon/state.json 2>/dev/null | grep -q '"pid"'; then
  echo "  FAIL  Agent B daemon did not start"
  echo "  Last 5 lines of log:"
  tail -5 $AGENT_B/daemon.log 2>/dev/null | sed 's/^/    /'
  exit 1
fi
echo "  OK    Agent B daemon running"

# Skip storage_module for B — LevelDB lock conflict with A
for m in capability_module lez_wallet_module delivery_module pilot; do
  timeout 15 $LOGOSCORE $CFG_B load-module $m > /dev/null 2>&1
done
sleep 3
echo "  OK    Agent B modules loaded"
echo ""

# ═══════════════════════════════════════
echo "── Phase 1: Initialize Both Agents ──"

R=$(call_a initialize $AGENT_A)
check "[A] initialize" "$R"

R=$(call_b initialize $AGENT_B)
check "[B] initialize" "$R"

NPK_A=$(call_a getAgentNpk)
check "[A] getAgentNpk" "$NPK_A"

NPK_B=$(call_b getAgentNpk)
check "[B] getAgentNpk" "$NPK_B"

ACCOUNT_A=$(call_a getAccountId)
ACCOUNT_B=$(call_b getAccountId)

# Extract viewing keys for encryption
VIEW_A=$(echo "$NPK_A" | python3 -c "import sys,json; print(json.load(sys.stdin).get('viewing_public_key',''))" 2>/dev/null)
VIEW_B=$(echo "$NPK_B" | python3 -c "import sys,json; print(json.load(sys.stdin).get('viewing_public_key',''))" 2>/dev/null)

echo ""
echo "  Agent A: $(echo $ACCOUNT_A | head -c 24)..."
echo "  Agent B: $(echo $ACCOUNT_B | head -c 24)..."
echo ""

# ═══════════════════════════════════════
echo "── Phase 2: Agent Discovery ──"

R=$(call_a agentCard)
check "[A] publishes Agent Card" "$R"

sleep 3

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

R=$(call_b agentTask "$VIEW_A" "echo" '{"input":"cross-agent-test"}')
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
timeout 5 $LOGOSCORE $CFG_B stop > /dev/null 2>&1
pkill -9 -f logos_host_qt 2>/dev/null
rm -f ~/.cache/storage/dht/providers/LOCK

echo ""
echo "  Agent A log: $AGENT_A/daemon.log"
echo "  Agent B log: $AGENT_B/daemon.log"

exit $FAIL
