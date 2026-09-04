#!/usr/bin/env bash
# Shared steps for deploying a Pilot agent against the PUBLIC LEZ testnet: build, install, boot a
# logoscore daemon, load pilot, wait for the agent to fund itself from the faucet, and the JSON /
# chain-read helpers every script needs. Sourced by agents/deploy-agent.sh (and meant to be
# sourced by demo.sh); nothing here runs on its own.
#
# Every function is asserted the same way demo.sh asserts: it either does what it says or calls
# fail, which prints the daemon log tails and exits 1. No best-effort, no silent skip.
#
# Env the caller may set before sourcing (defaults match demo.sh):
#   LOGOSCORE_BIN, LGPM_BIN   pre-built binaries (CI passes pinned store paths; a clean clone builds)
#   LEZ_RPC                   sequencer / testnet JSON-RPC endpoint (default the public testnet)
#   RISC0_DEV_MODE            default 1 (public rail needs no proof)
#   FUND_TIMEOUT_SECS         budget for wallet sync + faucet claim per agent (default 4500)
#   FAIL_LOGS                 space-separated daemon logs to tail when something fails

export NIX_CONFIG="experimental-features = nix-command flakes"
export RISC0_DEV_MODE="${RISC0_DEV_MODE:-1}"
LEZ_RPC="${LEZ_RPC:-https://testnet.lez.logos.co}"
export PILOT_SEQUENCER_ADDR="$LEZ_RPC"
export PILOT_CHAIN_WAIT_SECS="${PILOT_CHAIN_WAIT_SECS:-600}"     # public testnet seals a block every ~60 s
export PILOT_SYNC_WAIT_SECS="${PILOT_SYNC_WAIT_SECS:-3600}"
export PILOT_TX_TIMEOUT_MS="${PILOT_TX_TIMEOUT_MS:-300000}"
export PILOT_NAT="${PILOT_NAT:-extip:127.0.0.1}"
FUND_TIMEOUT_SECS="${FUND_TIMEOUT_SECS:-4500}"
FAIL_LOGS="${FAIL_LOGS:-}"
T_START="${T_START:-$(date +%s)}"
LC="${LC:-}"; LGPM="${LGPM:-}"

fail() {                                   # every assertion goes through here: say what, show the logs, exit 1
  echo; echo "${FAIL_PREFIX:-FAIL} [$1]: $2"
  for l in $FAIL_LOGS; do
    echo "--- $(basename "$(dirname "$l")")/$(basename "$l") tail ---"; tail -25 "$l" 2>/dev/null
  done
  echo "-----------------------"
  exit 1
}
need() { command -v "$1" >/dev/null 2>&1 || fail prereq "missing tool: $1"; }
elapsed() { echo "$(( ($(date +%s) - T_START) / 60 ))m"; }

# The daemon wraps every module reply as {"result":"<module JSON as a STRING>"} — quote-escaped,
# so a naive grep for "key":"value" never matches. Unwrap once, read fields with python.
res()  { python3 -c 'import sys,json
try:
    d=json.load(sys.stdin); r=d.get("result","")
    print(r if isinstance(r,str) else json.dumps(r))
except Exception: print("")' 2>/dev/null; }
field(){ python3 -c 'import sys,json
try: o=json.loads(sys.stdin.read() or "{}")
except Exception: o={}
for k in sys.argv[1].split("."):
    o=o.get(k,"") if isinstance(o,dict) else ""
print(o if not isinstance(o,(dict,list)) else json.dumps(o))' "$1" 2>/dev/null; }
rpc()  { curl -s -m 30 -X POST "$LEZ_RPC" -H 'content-type: application/json' \
           -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
b58()  { python3 -c 'import sys
A="123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"; b=bytes.fromhex(sys.argv[1]); n=int.from_bytes(b,"big"); s=""
while n: n,r=divmod(n,58); s=A[r]+s
print("1"*(len(b)-len(b.lstrip(b"\x00")))+s)' "$1"; }
acct() {  # hex id -> "balance nonce" read from the chain ("" if unknown)
  rpc getAccount "[\"$(b58 "$1")\"]" | python3 -c 'import sys,json
r=(json.load(sys.stdin).get("result") or {}); print(r.get("balance",""), r.get("nonce",""))' 2>/dev/null; }
tx_block() {  # tx hash -> block height, or "" when the chain does not know the hash
  rpc getTransaction "[\"$1\"]" | python3 -c 'import sys,json
r=json.load(sys.stdin).get("result"); print(r[1] if isinstance(r,list) and len(r)>1 else ("found" if r else ""))' 2>/dev/null; }

chain_check() {
  local H; H=$(rpc checkHealth '[]')
  echo "$H" | grep -q '"jsonrpc"' || fail chain "no JSON-RPC answer from $LEZ_RPC"
  echo "      chain answers: $(echo "$H" | head -c 80)"
}

# Build the runtime + pilot + its 3 dependency modules into $1 (a work dir). Sets LC, LGPM,
# LOGOS_HOST_PATH, LOGOS_BLOCKCHAIN_CIRCUITS. Same revisions as pilot-module/flake.lock and CI.
build_all() {
  local WORK="$1" ROOT="$2"
  if [ -n "${LOGOSCORE_BIN:-}" ] && [ -n "${LGPM_BIN:-}" ]; then
    LC="$LOGOSCORE_BIN"; LGPM="$LGPM_BIN"; echo "      using pre-built logoscore + lgpm from the environment"
  else
    nix build 'github:logos-co/logos-logoscore-cli' -o "$WORK/r-logoscore" || fail build "logoscore runtime"
    nix build 'github:logos-co/logos-package-manager' -o "$WORK/r-lgpm"     || fail build "lgpm installer"
    LC="$WORK/r-logoscore/bin/logoscore"; LGPM="$WORK/r-lgpm/bin/lgpm"
  fi
  ( cd "$ROOT/pilot-module" && nix build .#lgx -o "$WORK/r-pilot" ) || fail build "pilot module"
  # 549cf115 = execution-zone v0.2.2, the wallet generation whose program IDs match the public
  # testnet (verified byte-for-byte 2026-08-29).
  nix build 'github:logos-blockchain/logos-execution-zone-module/549cf1159f20fa0c3fe8e88a5ab71de68a5aa34b#lgx' -o "$WORK/r-ez"       || fail build "lez_core module"
  nix build 'github:logos-co/logos-delivery-module/3258cdb0132e37228aa2519e0c01c0e7429a20dd#lgx'               -o "$WORK/r-delivery" || fail build "delivery_module"
  nix build 'github:logos-co/logos-storage-module/7307910f0e5728e08c77820e21698e903c73d987#lgx'                -o "$WORK/r-storage"  || fail build "storage_module"
  export LOGOS_HOST_PATH="$(find /nix/store -maxdepth 1 -name '*-logos-liblogos' -type d 2>/dev/null | head -1)/bin/logos_host"
  [ -x "$LOGOS_HOST_PATH" ] || fail build "logos_host not found in the nix store (LOGOS_HOST_PATH)"
  local CIRCUITS; CIRCUITS="$(find /nix/store -maxdepth 1 -type d -name '*logos-blockchain-circuits*' 2>/dev/null | head -1)"
  [ -n "$CIRCUITS" ] && export LOGOS_BLOCKCHAIN_CIRCUITS="$CIRCUITS"
  echo "      built ($(elapsed))"
}

# Install the four .lgx packages built by build_all into the modules dir $2.
install_modules() {
  local WORK="$1" MODS="$2"
  for f in "$WORK"/r-pilot/*.lgx "$WORK"/r-ez/*.lgx "$WORK"/r-delivery/*.lgx "$WORK"/r-storage/*.lgx; do
    "$LGPM" install --file "$f" --modules-dir "$MODS" --allow-unsigned >/dev/null 2>&1 || fail install "$f"
  done
  echo "      installed: $(ls "$MODS" | tr '\n' ' ')"
}

# start_daemon <config-dir> <modules-dir> <log> [VAR=value ...]
# Boots one logoscore daemon with its own config dir (its RPC socket and state live there, so
# several daemons coexist on one host) and the given extra env for the modules it spawns.
start_daemon() {
  local LCDIR="$1" MODS="$2" LOG="$3"; shift 3
  mkdir -p "$LCDIR"
  setsid env "$@" "$LC" --config-dir "$LCDIR" -D -m "$MODS" </dev/null >"$LOG" 2>&1 & disown
  for i in $(seq 1 40); do grep -q '"pid"' "$LCDIR/daemon/state.json" 2>/dev/null && return 0; sleep 2; done
  fail daemon "daemon with config dir $LCDIR did not start within 80 s"
}

stop_daemon() {
  local LCDIR="$1"
  [ -n "$LC" ] && "$LC" --config-dir "$LCDIR" stop >/dev/null 2>&1 || true
  pkill -f "logoscore --config-dir $LCDIR" >/dev/null 2>&1 || true
}

# load_pilot <config-dir> <log>: load pilot and assert the daemon's own record of its 3 deps.
load_pilot() {
  local LCDIR="$1" LOG="$2" LOAD
  LOAD=$("$LC" --config-dir "$LCDIR" load-module pilot 2>&1)
  echo "$LOAD" | grep -q '"status":"ok"' || fail load "pilot did not load ($LCDIR): $LOAD"
  for dep in lez_core delivery_module storage_module pilot; do
    grep -q "Module loaded: $dep" "$LOG" || fail load "$dep never came up ($LCDIR)"
  done
}

# call <config-dir> <method> [args...]: one module call, unwrapped to the module's JSON.
# The daemon abandons a call at ~20 s while the module keeps working; callers that expect a
# long-running method poll the observable result (a file, a status field) instead.
call() { local LCDIR="$1"; shift; "$LC" --config-dir "$LCDIR" call pilot "$@" 2>/dev/null | res; }

# wait_funded <config-dir> <data-dir> <name>: initialize (identity + self-funding from the
# faucet), then poll metaStatus until a public account is credited ON CHAIN. Sets PUB (hex id),
# PUB_B58, PBAL, PNONCE, ST (last status JSON), ACCOUNT (private account). Counted in POLLS, not
# wall-clock seconds (a machine that sleeps mid-run must not expire the budget on resume).
wait_funded() {
  local LCDIR="$1" DATA="$2" NAME="${3:-}" TAG=""
  [ -n "$NAME" ] && TAG="[$NAME] "
  mkdir -p "$DATA"
  "$LC" --config-dir "$LCDIR" call pilot initialize "$DATA" >/dev/null 2>&1 || true
  PUB=""; ST=""; local POLLS=$(( FUND_TIMEOUT_SECS / 20 )) i=0 ERR
  while [ "$i" -lt "$POLLS" ]; do
    i=$(( i + 1 ))
    ST=$(call "$LCDIR" metaStatus)
    PUB=$(echo "$ST" | field balance.public_account)
    if [ ${#PUB} -eq 64 ]; then break; fi
    ERR=$(echo "$ST" | field funding.last_error)
    case "$ERR" in *"never credited"*|*"claim_pinata failed"*|*"register_public_account failed"*|*"create_account_public failed"*|*"unsolvable"*)
      fail fund "${TAG}funding stopped before a public account was credited: $ERR";; esac
    # A status reply means initialize has returned; none means the module thread is still inside
    # the wallet sync / funding (the daemon abandons the call at ~20 s). Say so every ~5 minutes.
    if [ $(( i % 15 )) -eq 0 ]; then
      if [ -n "$ST" ]; then echo "      … ${TAG}poll $i/$POLLS ($(elapsed)): funded=$(echo "$ST" | field funding.funded) last_error=$(echo "$ST" | field funding.last_error | head -c 80)"
      else echo "      … ${TAG}poll $i/$POLLS ($(elapsed)): module busy (syncing / funding), no status yet"; fi
    fi
    sleep 20
  done
  [ ${#PUB} -eq 64 ] || fail fund "${TAG}no funded public account after $POLLS polls (last status: $(echo "$ST" | head -c 300))"
  read -r PBAL PNONCE <<<"$(acct "$PUB")"
  # The claim credits 150. With real proofs the module finishes the shielded leg (100 out) INSIDE
  # initialize, before metaStatus ever answers, so the first reading may already be 50 (measured on
  # a 16 GB runner 2026-09-04, run 33880084026): funded either way.
  [ -n "$PBAL" ] && [ "${PNONCE:-0}" -ge 1 ] && [ "$PBAL" -ge 50 ] \
    || fail fund "${TAG}chain does not show the claimed account funded: $(b58 "$PUB") balance='$PBAL' nonce='$PNONCE'"
  PUB_B58=$(b58 "$PUB")
  ACCOUNT=$(echo "$ST" | field account)
  echo "      ${TAG}agent private account: $ACCOUNT"
  echo "      ${TAG}funded public account: $PUB_B58 = $PBAL LEZ on chain (nonce $PNONCE)  [$(elapsed)]"
}

# wait_quiet <config-dir> <name>: the module keeps working after initialize returns (funding,
# first sync); a call fired into that window is abandoned by the daemon. Wait until it answers.
wait_quiet() {
  local LCDIR="$1" NAME="$2" Q
  for i in $(seq 1 30); do
    Q=$(call "$LCDIR" getAccountId); [ -n "$Q" ] && { echo "      [$NAME] quiet and answering"; return 0; }
    sleep 10
  done
  fail quiet "[$NAME] never answered getAccountId after funding"
}

# ---- Waku relay for the two-agent roles ---------------------------------------------------------
# Messaging (and the share message of the storage role) ride Logos Messaging. The agents dial one
# local nwaku relay and the pull path reads that relay's REST store — nothing depends on the
# public fleet. Sets PILOT_WAKU_ADDR / PILOT_WAKU_REST for the daemons started afterwards.
start_relay() {
  local REST="http://127.0.0.1:8645" ID=""
  if ! curl -s -m 3 "$REST/debug/v1/info" >/dev/null 2>&1; then
    need docker
    docker rm -f pilot-nwaku >/dev/null 2>&1 || true
    docker run -d --name pilot-nwaku \
      -p 127.0.0.1:8645:8645 -p 30303:30303/tcp -p 30303:30303/udp \
      harbor.status.im/wakuorg/nwaku:v0.38.0 \
      --tcp-port=30303 --rest=true --rest-address=0.0.0.0 --rest-port=8645 --rest-admin=true \
      --relay=true --cluster-id=2 --num-shards-in-network=8 \
      --store=true --store-message-db-url=sqlite:///store.sqlite3 \
      --discv5-discovery=false --nat=extip:127.0.0.1 >/dev/null || fail relay "could not start the nwaku container"
    for i in $(seq 1 30); do curl -s -m 3 "$REST/debug/v1/info" >/dev/null 2>&1 && break; sleep 2; done
  fi
  ID=$(curl -s -m 5 "$REST/debug/v1/info" | python3 -c \
    "import sys,json;print(json.load(sys.stdin)['listenAddresses'][0].rsplit('/p2p/',1)[1])" 2>/dev/null)
  [ -n "$ID" ] || fail relay "nwaku is not answering on $REST"
  # A COMPLETE multiaddr (with /p2p/<id>): without the peer id the peer manager never dials it.
  export PILOT_WAKU_ADDR="/ip4/127.0.0.1/tcp/30303/p2p/$ID"
  export PILOT_WAKU_REST="$REST"
  echo "      relay: $PILOT_WAKU_ADDR"
}

stop_relay() { docker rm -f pilot-nwaku >/dev/null 2>&1 || true; }
