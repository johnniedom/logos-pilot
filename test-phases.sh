#!/bin/bash
set -o pipefail

LOGOSCORE=$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f 2>/dev/null | head -1)
export LOGOS_HOST_PATH=$(find /nix/store -maxdepth 3 -name logos_host -path "*liblogos-bin*" -type f 2>/dev/null | head -1)
CFG="--config-dir /tmp/pilot-test/.logoscore"
DATA="/tmp/pilot-test"
MODULES="${PILOT_MODULE_PATH:-$HOME/.pilot/modules}"

PASS=0
FAIL=0
SKIP=0

check() {
  local phase="$1" method="$2" result="$3"
  if echo "$result" | grep -q '"error"'; then
    echo "  FAIL  [$phase] $method"
    echo "        → $(echo "$result" | head -c 120)"
    ((FAIL++))
  elif [ -z "$result" ]; then
    echo "  FAIL  [$phase] $method (empty response)"
    ((FAIL++))
  else
    echo "  PASS  [$phase] $method"
    ((PASS++))
  fi
}

daemon_alive() {
  local pid=$(cat $DATA/.logoscore/daemon/state.json 2>/dev/null | grep -o '"pid": [0-9]*' | grep -o '[0-9]*')
  if [ -z "$pid" ] || ! kill -0 $pid 2>/dev/null; then
    echo ""
    echo "  ✗ DAEMON CRASHED — stopping test"
    echo "    Last 5 lines of daemon.log:"
    tail -5 $DATA/daemon.log 2>/dev/null | sed 's/^/    /'
    echo ""
    echo "══════════════════════════════════════"
    echo "  Results: $PASS passed, $FAIL failed (daemon died)"
    echo "══════════════════════════════════════"
    exit 1
  fi
}

echo "╔══════════════════════════════════════════════╗"
echo "║        PILOT PHASE INTEGRATION TEST         ║"
echo "╚══════════════════════════════════════════════╝"
echo ""

# Pre-flight checks
if ! curl -s -o /dev/null -w "" http://127.0.0.1:3040/ 2>/dev/null; then
  echo "  ✗ Sequencer not running on port 3040"
  echo "    Run ./run-sequencer.sh first"
  exit 1
fi
echo "  ✓ Sequencer reachable"

if [ ! -d "$MODULES" ] || [ -z "$(ls -A $MODULES 2>/dev/null)" ]; then
  echo "  ✗ No modules in $MODULES"
  echo "    Run ./setup-modules.sh first"
  exit 1
fi
echo "  ✓ Modules installed ($(ls $MODULES | wc -l) modules)"
echo ""

# Clean
pkill -9 -f logos_host_qt 2>/dev/null
pkill -9 -f logoscore 2>/dev/null
rm -rf /tmp/pilot-test ~/.cache/storage/dht/providers/LOCK
mkdir -p $DATA
sleep 1

echo "── Starting daemon ──"
setsid $LOGOSCORE $CFG -D -m $MODULES > $DATA/daemon.log 2>&1 &
sleep 5
if ! cat $DATA/.logoscore/daemon/state.json 2>/dev/null | grep -q '"pid"'; then
  echo "  FAIL  Daemon did not start"
  exit 1
fi
echo "  OK    Daemon running ($(cat $DATA/.logoscore/daemon/state.json | grep -o '"pid": [0-9]*'))"
echo ""

# Load modules
echo "── Loading modules ──"
for m in capability_module lez_core delivery_module storage_module pilot; do
  timeout 15 $LOGOSCORE $CFG load-module $m > /dev/null 2>&1
  echo "  OK    $m"
done
sleep 3
echo ""

call() {
  timeout 20 $LOGOSCORE $CFG call pilot "$@" 2>&1 | grep -o '{.*}' | tail -1
}

extract() {
  echo "$1" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('result',''))" 2>/dev/null || echo "$1"
}

# ═══════════════════════════════════════
echo "── Phase 1: Identity + Wallet ──"

R=$(call initialize $DATA)
check "P1" "initialize" "$(extract "$R")"

R=$(call getAgentNpk)
NPK=$(extract "$R")
check "P1" "getAgentNpk" "$NPK"

R=$(call getAccountId)
ACCOUNT=$(extract "$R")
check "P1" "getAccountId" "$ACCOUNT"

R=$(call walletBalance)
check "P1" "walletBalance" "$(extract "$R")"

R=$(call walletHistory)
check "P1" "walletHistory" "$(extract "$R")"

echo ""

daemon_alive

# ═══════════════════════════════════════
echo "── Phase 2: Owner Channel ──"

# Set owner NPK (extract hex key from JSON, or use a dummy 64-char hex)
OWNER_KEY=$(echo "$NPK" | python3 -c "import sys,json; print(json.load(sys.stdin).get('nullifier_public_key',''))" 2>/dev/null)
if [ -z "$OWNER_KEY" ]; then OWNER_KEY="0000000000000000000000000000000000000000000000000000000000000001"; fi
call metaConfigure owner.npk "$OWNER_KEY" > /dev/null 2>&1

R=$(call establishOwnerChannel)
check "P2" "establishOwnerChannel" "$(extract "$R")"

R=$(call getOwnerChannelId)
CHANNEL=$(extract "$R")
check "P2" "getOwnerChannelId" "$CHANNEL"

R=$(call sendToOwner "test message from phase 2")
check "P2" "sendToOwner" "$(extract "$R")"

echo ""

daemon_alive

# ═══════════════════════════════════════
echo "── Phase 3: Spending FSM ──"

R=$(call setSpendingLimits 50 200 86400)
check "P3" "setSpendingLimits" "$(extract "$R")"

R=$(call createSpendRequest "$ACCOUNT" 10 "test spend")
SPEND_ID=$(extract "$R")
check "P3" "createSpendRequest" "$SPEND_ID"

R=$(call getPendingSpends)
check "P3" "getPendingSpends" "$(extract "$R")"

R=$(call approveSpend "$SPEND_ID")
check "P3" "approveSpend" "$(extract "$R")"

R=$(call walletSend "$ACCOUNT" 5 "below threshold")
check "P3" "walletSend (below threshold)" "$(extract "$R")"

echo ""

daemon_alive

# ═══════════════════════════════════════
echo "── Phase 4: Storage ──"

echo "test file content" > /tmp/pilot-test-file.txt

R=$(call storageUpload /tmp/pilot-test-file.txt "test-label")
UPLOAD_RESULT=$(extract "$R")
check "P4" "storageUpload" "$UPLOAD_RESULT"
CID=$(echo "$UPLOAD_RESULT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('cid','test-cid'))" 2>/dev/null || echo "test-cid")

R=$(call storageList)
check "P4" "storageList" "$(extract "$R")"

R=$(call storageDownload "$CID" /tmp/pilot-test-download.txt)
check "P4" "storageDownload" "$(extract "$R")"

R=$(call storageShare "$CID" "$NPK")
check "P4" "storageShare" "$(extract "$R")"

echo ""

daemon_alive

# ═══════════════════════════════════════
echo "── Phase 4: Messaging ──"

R=$(call messagingSend "$ACCOUNT" "hello from test")
check "P4" "messagingSend" "$(extract "$R")"

R=$(call messagingJoin "test-group-id")
check "P4" "messagingJoin" "$(extract "$R")"

R=$(call messagingCreateGroup "[\"$ACCOUNT\"]")
check "P4" "messagingCreateGroup" "$(extract "$R")"

echo ""

daemon_alive

# ═══════════════════════════════════════
echo "── Phase 5: A2A Transport ──"

R=$(call agentCard)
check "P5" "agentCard" "$(extract "$R")"

R=$(call agentDiscover "pilot")
check "P5" "agentDiscover" "$(extract "$R")"

R=$(call agentTask "$ACCOUNT" "echo" "{\"input\":\"test\"}")
check "P5" "agentTask" "$(extract "$R")"

R=$(call agentSubscribe "$ACCOUNT" "task-123")
check "P5" "agentSubscribe" "$(extract "$R")"

R=$(call agentCancel "$ACCOUNT" "task-123")
check "P5" "agentCancel" "$(extract "$R")"

echo ""

daemon_alive

# ═══════════════════════════════════════
echo "── Phase 4+: Meta Skills ──"

R=$(call metaSkills)
check "META" "metaSkills" "$(extract "$R")"

R=$(call metaStatus)
check "META" "metaStatus" "$(extract "$R")"

R=$(call metaConfigure "test.key" "test.value")
check "META" "metaConfigure" "$(extract "$R")"

echo ""

# ═══════════════════════════════════════
echo "══════════════════════════════════════"
echo "  Results: $PASS passed, $FAIL failed"
echo "══════════════════════════��═══════════"

# Cleanup
timeout 5 $LOGOSCORE $CFG stop > /dev/null 2>&1
pkill -9 -f logos_host_qt 2>/dev/null

exit $FAIL
