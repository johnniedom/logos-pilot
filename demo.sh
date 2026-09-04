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
# The build / install / boot / fund steps and the JSON + chain-read helpers live in agents/lib.sh, shared
# with agents/deploy-agent.sh (the per-category testnet agents) so there is one copy of each.
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
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FAIL_PREFIX="DEMO FAIL"
# shellcheck source=agents/lib.sh
. "$ROOT/agents/lib.sh"
DEMO_RECIPIENT_HEX="${DEMO_RECIPIENT_HEX:-f8fc394c0e5440c4188236d1693076b0cfad04984cf67ca64e0e43a173144f63}"
DEMO_AMOUNT="${DEMO_AMOUNT:-1}"

WORK="$(mktemp -d)"
MODS="$WORK/modules"; DATA="$WORK/data"; LCDIR="$WORK/logoscore"
mkdir -p "$MODS" "$DATA" "$LCDIR"
FAIL_LOGS="$WORK/daemon.log"

cleanup() {
  stop_daemon "$LCDIR"
  # Keep the daemon log next to the repo: it carries the wallet host's output (sync progress, and
  # with RISC0_DEV_MODE=0 the prover's own lines), which is the evidence a reviewer wants to see.
  [ -f "$WORK/daemon.log" ] && cp "$WORK/daemon.log" "$ROOT/demo-daemon.log" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

need nix; need python3; need curl

echo "=== LP-0008 Pilot — end-to-end demo  (endpoint $LEZ_RPC, RISC0_DEV_MODE=$RISC0_DEV_MODE) ==="
chain_check

# Optional RISC0 toolchain (needed only for real proofs on the shielded leg); discover if present.
R0VM_DIR="$(find "$HOME/.risc0/extensions" -maxdepth 1 -type d -name '*cargo-risczero*' 2>/dev/null | head -1)"
[ -n "$R0VM_DIR" ] && export PATH="$R0VM_DIR:$PATH"

echo "[1/6] Building runtime + module + dependency modules (nix; pinned revisions)..."
build_all "$WORK" "$ROOT"

echo "[2/6] Installing pilot + 3 dependency modules via lgpm, starting the logoscore daemon, loading pilot..."
install_modules "$WORK" "$MODS"
start_daemon "$LCDIR" "$MODS" "$WORK/daemon.log"
load_pilot "$LCDIR" "$WORK/daemon.log"
echo "      daemon up; pilot + lez_core + delivery_module + storage_module loaded"

echo "[3/6] Skills + echo round-trip..."
COUNT=$(call "$LCDIR" metaSkills | field count)
[ "$COUNT" = "23" ] || fail skills "expected 23 registered skills, got '$COUNT'"
ECHO=$(call "$LCDIR" echo "clean-clone-demo")
[ "$ECHO" = "echo: clean-clone-demo" ] || fail echo "echo round-trip returned '$ECHO'"
echo "      23 skills registered; echo round-trip ok"

echo "[4/6] Identity + self-funding from the faucet (initialize; the daemon call times out at ~20 s while the"
echo "      module keeps working — a cold wallet replays the chain first, then registers, claims, persists)..."
# Polls, not wall-clock seconds; the chain, not the agent's say-so. Sets PUB, ST, PBAL, PNONCE.
wait_funded "$LCDIR" "$DATA"
FUNDED=$(echo "$ST" | field funding.funded); LASTERR=$(echo "$ST" | field funding.last_error)
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
    ST=$(call "$LCDIR" metaStatus)
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
SEND=$(call "$LCDIR" walletSend "public:$DEMO_RECIPIENT_HEX" "$DEMO_AMOUNT" "demo: public transfer through the spending FSM")
STATUS=$(echo "$SEND" | field status); REQ=$(echo "$SEND" | field request_id)
[ "$STATUS" = "completed" ] || fail spend "walletSend answered '$SEND'"
TX=$(call "$LCDIR" walletHistory | python3 -c 'import sys,json
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
CID=$(call "$LCDIR" storageUpload "$WORK/vault.txt" demo-doc | field cid)
[ -n "$CID" ] || fail vault "storage.upload returned no CID"
call "$LCDIR" storageDownload "$CID" "$WORK/vault.out" >/dev/null
cmp -s "$WORK/vault.txt" "$WORK/vault.out" || fail vault "downloaded file differs from the upload (cid $CID)"
echo "      cid $CID uploaded encrypted and downloaded back byte-identical"

echo
echo "=== DEMO PASSED in $(elapsed): build + load + 23 skills + echo; self-funded from the faucet ($(b58 "$PUB"));"
echo "    spent $DEMO_AMOUNT LEZ through the spending FSM, settled on chain (tx $TXH, block $BLK); vault round-trip identical. ==="
