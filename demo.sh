#!/usr/bin/env bash
# LP-0008 Pilot — reproducible end-to-end demo against the PUBLIC LEZ testnet, from a clean clone.
#
#   ./demo.sh                                  # public testnet (default); every step asserted; exit 1 on failure
#   LEZ_RPC=http://127.0.0.1:3040 ./demo.sh    # a local sequencer instead (see run-sequencer-realproof.sh)
#
# Every step below is ASSERTED. There is no "best-effort" and no silent skip: the script exits non-zero
# the moment a step does not do what it claims, and prints the daemon log tail so the failure is visible.
#
#   1. build   pilot module + its 3 dependency modules (+ the logoscore runtime and lgpm installer)
#   2. load    the real logoscore daemon loads pilot and brings up lez_core / delivery_module / storage_module
#   3. skills  23 skills registered; echo round-trip through the daemon
#   4. fund    initialize creates the agent's shielded account, then the agent funds ITSELF: registers a
#              public account, solves the faucet's proof-of-work, claims it, and persists that account.
#              Verified by reading the account FROM THE CHAIN (getAccount), not from the agent's own say-so.
#   5. spend   pilot.walletSend to public:<recipient> — the agent's own spending FSM (limits, request row,
#              terminal state + tx hash). Verified by getTransaction on the returned hash and by the
#              recipient's chain balance rising by the amount.
#   6. vault   storage.upload of a file, storage.download to a second path, byte-identical.
#
# Proofs. A public transfer is signed by the client and proven by the sequencer, so steps 4-5 need NO
# client-side RISC0 proof and finish in minutes on any machine (measured 2026-09-04 on the public testnet:
# tx 9cddfa1a…4bd7, block 36607). The shielded leg of funding (public -> the agent's private account) DOES
# need a real proof: the public testnet does not mine dev-mode receipts, and a real proof wants ~16 GB RAM
# (KNOWN_LIMITATIONS.md §5). That leg's outcome is printed honestly and is NOT a pass/fail condition here;
# run with RISC0_DEV_MODE=0 on a big enough machine to see it land too.
#
# Env overrides:
#   LOGOSCORE_BIN, LGPM_BIN   pre-built binaries (CI passes pinned store paths; a clean clone builds them)
#   LEZ_RPC                   sequencer / testnet JSON-RPC endpoint (default https://testnet.lez.logos.co)
#   DEMO_RECIPIENT_HEX        32-byte hex id of a REGISTERED public account to pay (default: a project test
#                             account on the public testnet; on a local chain you must supply one)
#   DEMO_AMOUNT               LEZ to send (default 1)
#   RISC0_DEV_MODE            default 1 (public rail needs no proof); 0 for real proofs on the shielded leg
#   FUND_TIMEOUT_SECS         budget for wallet sync + faucet claim (default 4500: a cold wallet replays the
#                             whole chain first, ~1,000-3,000 blocks/min)

set -uo pipefail
export NIX_CONFIG="experimental-features = nix-command flakes"
export RISC0_DEV_MODE="${RISC0_DEV_MODE:-1}"
LEZ_RPC="${LEZ_RPC:-https://testnet.lez.logos.co}"
export PILOT_SEQUENCER_ADDR="$LEZ_RPC"
export PILOT_CHAIN_WAIT_SECS="${PILOT_CHAIN_WAIT_SECS:-600}"     # public testnet seals a block every ~60 s
export PILOT_SYNC_WAIT_SECS="${PILOT_SYNC_WAIT_SECS:-3600}"
export PILOT_TX_TIMEOUT_MS="${PILOT_TX_TIMEOUT_MS:-300000}"     # shielded leg ceiling; raise for real proofs
export PILOT_NAT="${PILOT_NAT:-extip:127.0.0.1}"
DEMO_RECIPIENT_HEX="${DEMO_RECIPIENT_HEX:-f8fc394c0e5440c4188236d1693076b0cfad04984cf67ca64e0e43a173144f63}"
DEMO_AMOUNT="${DEMO_AMOUNT:-1}"
FUND_TIMEOUT_SECS="${FUND_TIMEOUT_SECS:-4500}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
MODS="$WORK/modules"; DATA="$WORK/data"; LCDIR="$WORK/logoscore"
mkdir -p "$MODS" "$DATA" "$LCDIR"
LC=""; LCCFG=""
T_START=$(date +%s)

cleanup() {
  if [ -n "$LC" ]; then "$LC" $LCCFG stop >/dev/null 2>&1 || true; fi
  pkill -f "logoscore --config-dir $LCDIR" >/dev/null 2>&1 || true
  # Keep the daemon log next to the repo: it carries the wallet host's output (sync progress, and
  # with RISC0_DEV_MODE=0 the prover's own lines), which is the evidence a reviewer wants to see.
  [ -f "$WORK/daemon.log" ] && cp "$WORK/daemon.log" "$ROOT/demo-daemon.log" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

fail() {                                   # every assertion goes through here: say what, show the log, exit 1
  echo; echo "DEMO FAIL [$1]: $2"
  echo "--- daemon log tail ---"; tail -25 "$WORK/daemon.log" 2>/dev/null; echo "-----------------------"
  exit 1
}
need() { command -v "$1" >/dev/null 2>&1 || fail prereq "missing tool: $1"; }
need nix; need python3; need curl

# The daemon wraps every module reply as {"result":"<module JSON as a STRING>"} — quote-escaped, so a naive
# grep for "key":"value" never matches. Unwrap it once, here, and read fields with python.
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
elapsed() { echo "$(( ($(date +%s) - T_START) / 60 ))m"; }

echo "=== LP-0008 Pilot — end-to-end demo  (endpoint $LEZ_RPC, RISC0_DEV_MODE=$RISC0_DEV_MODE) ==="
H=$(rpc checkHealth '[]'); echo "$H" | grep -q '"jsonrpc"' || fail chain "no JSON-RPC answer from $LEZ_RPC"
echo "      chain answers: $(echo "$H" | head -c 80)"

# Optional RISC0 toolchain (needed only for real proofs on the shielded leg); discover if present.
R0VM_DIR="$(find "$HOME/.risc0/extensions" -maxdepth 1 -type d -name '*cargo-risczero*' 2>/dev/null | head -1)"
[ -n "$R0VM_DIR" ] && export PATH="$R0VM_DIR:$PATH"

echo "[1/6] Building runtime + module + dependency modules (nix; pinned revisions)..."
if [ -n "${LOGOSCORE_BIN:-}" ] && [ -n "${LGPM_BIN:-}" ]; then
  LC="$LOGOSCORE_BIN"; LGPM="$LGPM_BIN"; echo "      using pre-built logoscore + lgpm from the environment"
else
  nix build 'github:logos-co/logos-logoscore-cli' -o "$WORK/r-logoscore" || fail build "logoscore runtime"
  nix build 'github:logos-co/logos-package-manager' -o "$WORK/r-lgpm"     || fail build "lgpm installer"
  LC="$WORK/r-logoscore/bin/logoscore"; LGPM="$WORK/r-lgpm/bin/lgpm"
fi
( cd "$ROOT/pilot-module" && nix build .#lgx -o "$WORK/r-pilot" ) || fail build "pilot module"
# Same revisions as pilot-module/flake.lock and .github/workflows/ci.yml. 549cf115 = execution-zone v0.2.2,
# the wallet generation whose program IDs match the public testnet (verified byte-for-byte 2026-08-29).
nix build 'github:logos-blockchain/logos-execution-zone-module/549cf1159f20fa0c3fe8e88a5ab71de68a5aa34b#lgx' -o "$WORK/r-ez"       || fail build "lez_core module"
nix build 'github:logos-co/logos-delivery-module/3258cdb0132e37228aa2519e0c01c0e7429a20dd#lgx'               -o "$WORK/r-delivery" || fail build "delivery_module"
nix build 'github:logos-co/logos-storage-module/7307910f0e5728e08c77820e21698e903c73d987#lgx'                -o "$WORK/r-storage"  || fail build "storage_module"
export LOGOS_HOST_PATH="$(find /nix/store -maxdepth 1 -name '*-logos-liblogos' -type d 2>/dev/null | head -1)/bin/logos_host"
[ -x "$LOGOS_HOST_PATH" ] || fail build "logos_host not found in the nix store (LOGOS_HOST_PATH)"
CIRCUITS="$(find /nix/store -maxdepth 1 -type d -name '*logos-blockchain-circuits*' 2>/dev/null | head -1)"
[ -n "$CIRCUITS" ] && export LOGOS_BLOCKCHAIN_CIRCUITS="$CIRCUITS"
echo "      built ($(elapsed))"

echo "[2/6] Installing pilot + 3 dependency modules via lgpm, starting the logoscore daemon, loading pilot..."
for f in "$WORK"/r-pilot/*.lgx "$WORK"/r-ez/*.lgx "$WORK"/r-delivery/*.lgx "$WORK"/r-storage/*.lgx; do
  "$LGPM" install --file "$f" --modules-dir "$MODS" --allow-unsigned >/dev/null 2>&1 || fail install "$f"
done
echo "      installed: $(ls "$MODS" | tr '\n' ' ')"
LCCFG="--config-dir $LCDIR"
setsid "$LC" $LCCFG -D -m "$MODS" </dev/null >"$WORK/daemon.log" 2>&1 & disown
for i in $(seq 1 30); do grep -q '"pid"' "$LCDIR/daemon/state.json" 2>/dev/null && break; sleep 2; done
LOAD=$("$LC" $LCCFG load-module pilot 2>&1)
echo "$LOAD" | grep -q '"status":"ok"' || fail load "pilot did not load: $LOAD"
for dep in lez_core delivery_module storage_module pilot; do   # the daemon's own record, not pilot's reply
  grep -q "Module loaded: $dep" "$WORK/daemon.log" || fail load "$dep never came up"
done
echo "      daemon up; pilot + lez_core + delivery_module + storage_module loaded"

echo "[3/6] Skills + echo round-trip..."
COUNT=$("$LC" $LCCFG call pilot metaSkills 2>/dev/null | res | field count)
[ "$COUNT" = "23" ] || fail skills "expected 23 registered skills, got '$COUNT'"
ECHO=$("$LC" $LCCFG call pilot echo "clean-clone-demo" 2>/dev/null | res)
[ "$ECHO" = "echo: clean-clone-demo" ] || fail echo "echo round-trip returned '$ECHO'"
echo "      23 skills registered; echo round-trip ok"

echo "[4/6] Identity + self-funding from the faucet (initialize; the daemon call times out at ~20 s while the"
echo "      module keeps working — a cold wallet replays the chain first, then registers, claims, persists)..."
"$LC" $LCCFG call pilot initialize "$DATA" >/dev/null 2>&1 || true
# The wait is counted in POLLS, not wall-clock seconds: a laptop that sleeps mid-demo freezes the
# daemon with it, and a wall-clock deadline would expire the moment it resumes (2026-09-04).
PUB=""; ST=""; POLLS=$(( FUND_TIMEOUT_SECS / 20 )); i=0
while [ "$i" -lt "$POLLS" ]; do
  i=$(( i + 1 ))
  ST=$("$LC" $LCCFG call pilot metaStatus 2>/dev/null | res)
  PUB=$(echo "$ST" | field balance.public_account)
  if [ ${#PUB} -eq 64 ]; then break; fi
  ERR=$(echo "$ST" | field funding.last_error)
  case "$ERR" in *"never credited"*|*"claim_pinata failed"*|*"register_public_account failed"*|*"create_account_public failed"*|*"unsolvable"*)
    fail fund "funding stopped before a public account was credited: $ERR";; esac
  # A status reply means initialize has returned; none means the module thread is still inside
  # the wallet sync / funding (the daemon abandons the call at ~20 s). Say so every ~5 minutes.
  if [ $(( i % 15 )) -eq 0 ]; then
    if [ -n "$ST" ]; then echo "      … poll $i/$POLLS ($(elapsed)): funded=$(echo "$ST" | field funding.funded) last_error=$(echo "$ST" | field funding.last_error | head -c 80)"
    else echo "      … poll $i/$POLLS ($(elapsed)): module busy (syncing / funding), no status yet"; fi
  fi
  sleep 20
done
[ ${#PUB} -eq 64 ] || fail fund "no funded public account after $POLLS polls (last status: $(echo "$ST" | head -c 300))"
read -r PBAL PNONCE <<<"$(acct "$PUB")"
# The claim credits 150. With real proofs the module finishes the shielded leg (100 out) INSIDE
# initialize, before metaStatus ever answers, so the first reading may already be 50 (measured on
# a 16 GB runner 2026-09-04, run 33880084026): the account is funded either way, and a balance
# of 50 with funding.funded set is the proof having landed, not a shortfall.
FUNDED=$(echo "$ST" | field funding.funded); LASTERR=$(echo "$ST" | field funding.last_error)
[ -n "$PBAL" ] && [ "${PNONCE:-0}" -ge 1 ] && [ "$PBAL" -ge 50 ] \
  || fail fund "chain does not show the claimed account funded: $(b58 "$PUB") balance='$PBAL' nonce='$PNONCE'"
echo "      agent private account: $(echo "$ST" | field account)"
echo "      funded public account: $(b58 "$PUB") = $PBAL LEZ on chain (nonce $PNONCE)  [$(elapsed)]"
if [ "$FUNDED" = "True" ] || [ "$FUNDED" = "true" ]; then
  [ "$PBAL" -le 50 ] && [ "${PNONCE:-0}" -ge 2 ] \
    || fail shielded "funding says funded but the chain shows the public account at $PBAL (nonce $PNONCE); 100 should have left it"
  echo "      shielded leg: LANDED ON CHAIN — 100 left the public account in its second transaction; private account funded (real proof accepted)"
elif [ "${REQUIRE_SHIELDED:-0}" = "1" ]; then
  # Strict mode (the real-proof CI job): the shielded leg — a real RISC0 proof, RISC0_DEV_MODE=0 —
  # must land too. The module keeps proving inside initialize; poll until funding.funded flips,
  # then trust the CHAIN, not the flag: the funded public account must have dropped by 100.
  echo "      shielded leg: waiting for the real proof (RISC0_DEV_MODE=$RISC0_DEV_MODE; polls counted, not wall-clock)..."
  SP=$(( ${SHIELDED_TIMEOUT_SECS:-16200} / 30 )); j=0
  while [ "$j" -lt "$SP" ]; do
    j=$(( j + 1 ))
    ST=$("$LC" $LCCFG call pilot metaStatus 2>/dev/null | res)
    FUNDED=$(echo "$ST" | field funding.funded); LASTERR=$(echo "$ST" | field funding.last_error)
    if [ "$FUNDED" = "True" ] || [ "$FUNDED" = "true" ]; then break; fi
    case "$LASTERR" in *"transfer_shielded_owned failed"*|*"never appeared"*) fail shielded "the module gave up on the shielded transfer: $LASTERR";; esac
    if [ $(( j % 10 )) -eq 0 ]; then echo "      … proving, poll $j/$SP ($(elapsed))"; fi
    sleep 30
  done
  [ "$FUNDED" = "True" ] || [ "$FUNDED" = "true" ] || fail shielded "the shielded transfer did not land within $SP polls"
  read -r PBAL_S PNONCE_S <<<"$(acct "$PUB")"
  [ -n "$PBAL_S" ] && [ "$PBAL_S" -le $(( PBAL - 100 )) ] \
    || fail shielded "funding says funded but the chain shows the public account at '$PBAL_S' (was $PBAL)"
  PBAL=$PBAL_S; PNONCE=$PNONCE_S
  echo "      shielded leg: LANDED ON CHAIN — public account now $PBAL (nonce $PNONCE); private account funded  [$(elapsed)]"
else
  echo "      shielded leg (needs a real proof; not a pass/fail condition here): ${LASTERR:-pending}"
fi

echo "[5/6] Spend through the agent's own spending FSM: pilot.walletSend public:<recipient> $DEMO_AMOUNT ..."
read -r RB0 RN0 <<<"$(acct "$DEMO_RECIPIENT_HEX")"
[ -n "$RB0" ] || fail spend "recipient $(b58 "$DEMO_RECIPIENT_HEX") is not a registered account on this chain (set DEMO_RECIPIENT_HEX)"
SEND=$("$LC" $LCCFG call pilot walletSend "public:$DEMO_RECIPIENT_HEX" "$DEMO_AMOUNT" "demo: public transfer through the spending FSM" 2>/dev/null | res)
STATUS=$(echo "$SEND" | field status); REQ=$(echo "$SEND" | field request_id)
[ "$STATUS" = "completed" ] || fail spend "walletSend answered '$SEND'"
TX=$("$LC" $LCCFG call pilot walletHistory 2>/dev/null | res | python3 -c 'import sys,json
req=sys.argv[1]
for t in json.loads(sys.stdin.read() or "{}").get("transactions",[]):
    if t.get("id")==req: print(t.get("tx_hash",""), t.get("state",""))' "$REQ")
read -r TXH TXS <<<"$TX"
[ "$TXS" = "COMPLETED" ] && [ ${#TXH} -eq 64 ] || fail spend "spend row $REQ is '$TXS' with hash '$TXH'"
echo "      request $REQ COMPLETED, tx $TXH — waiting for the chain to know it..."
BLK=""; for i in $(seq 1 20); do BLK=$(tx_block "$TXH"); [ -n "$BLK" ] && break; sleep 15; done
[ -n "$BLK" ] || fail spend "getTransaction($TXH) is still unknown to the chain after 5 min"
RB1=""; RN1=""
for i in $(seq 1 20); do read -r RB1 RN1 <<<"$(acct "$DEMO_RECIPIENT_HEX")"; [ "${RB1:-0}" -ge $(( RB0 + DEMO_AMOUNT )) ] && break; sleep 15; done
[ "${RB1:-0}" -ge $(( RB0 + DEMO_AMOUNT )) ] || fail spend "recipient balance did not rise: $RB0 -> ${RB1:-?}"
read -r PBAL1 PNONCE1 <<<"$(acct "$PUB")"
echo "      ON CHAIN: tx $TXH in block $BLK; recipient $RB0 -> $RB1; sender $PBAL -> $PBAL1 (nonce $PNONCE -> $PNONCE1)  [$(elapsed)]"

echo "[6/6] Encrypted file vault: storage.upload -> storage.download -> byte compare..."
echo "notarized secret $(date +%s)" > "$WORK/vault.txt"
CID=$("$LC" $LCCFG call pilot storageUpload "$WORK/vault.txt" demo-doc 2>/dev/null | res | field cid)
[ -n "$CID" ] || fail vault "storage.upload returned no CID"
"$LC" $LCCFG call pilot storageDownload "$CID" "$WORK/vault.out" >/dev/null 2>&1
cmp -s "$WORK/vault.txt" "$WORK/vault.out" || fail vault "downloaded file differs from the upload (cid $CID)"
echo "      cid $CID uploaded encrypted and downloaded back byte-identical"

echo
echo "=== DEMO PASSED in $(elapsed): build + load + 23 skills + echo; self-funded from the faucet ($(b58 "$PUB"));"
echo "    spent $DEMO_AMOUNT LEZ through the spending FSM, settled on chain (tx $TXH, block $BLK); vault round-trip identical. ==="
