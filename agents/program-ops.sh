#!/usr/bin/env bash
# LP-0008 — the program skills against the PUBLIC LEZ testnet, through the wallet module.
#
#   agents/program-ops.sh
#
# One agent (fresh identity, funded from the faucet). Then:
#   program.query   reads a program's on-chain account (the chain's own program ids, via
#                   getProgramIds) through the wallet module — balance, nonce, data, owner
#   program.deploy  submits a deployment transaction for the wallet module's built-in token
#                   program ELF (builtin:token) and reports the wallet's reply as it is
#   program.call    is NOT exercised here: it sends the instruction words the caller supplies and
#                   this script does not know any program's ABI; it is refused with usage when
#                   given nothing, which is asserted
# Every module-side step is asserted. The deploy's chain outcome is REPORTED, not assumed: a
# mined transaction is verified with getTransaction; a wallet-side refusal (for instance a program
# that already exists) is printed verbatim and the run still passes, because the point measured
# here is that the skill reaches the wallet and answers with the wallet's truth.

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib.sh
. "$ROOT/agents/lib.sh"
need nix; need python3; need curl

OUT="$ROOT/agents/out/programs"; mkdir -p "$OUT"
rm -f "$OUT"/agent-daemon.log "$OUT"/query.json "$OUT"/deploy.json
WORK="$(mktemp -d)"
MODS="$WORK/modules"; mkdir -p "$MODS"
A_LC="$WORK/a/lc"; A_DATA="$WORK/a/data"; A_LOG="$OUT/agent-daemon.log"
FAIL_LOGS="$A_LOG"
cleanup() { stop_daemon "$A_LC"; rm -rf "$WORK"; }
trap cleanup EXIT

echo "=== LP-0008 Pilot — program operations through the wallet module  (endpoint $LEZ_RPC) ==="
chain_check
# getProgramIds answers {"amm":[8 x u32], "authenticated_transfer":[8 x u32], ...}: RISC0 image
# ids as eight 32-bit words. Written as little-endian bytes they are the 64-hex account ids the
# wallet uses — authenticated_transfer's first word 583309054 = 0x22C496FE -> "fe96c422…", the
# program_owner every funded public account on this chain shows. Read that one (fallback: the
# first program listed).
PROGRAMS=$(rpc getProgramIds '[]')
read -r PROGRAM_NAME PROGRAM_ID <<<"$(echo "$PROGRAMS" | python3 -c 'import sys,json,struct
r=json.load(sys.stdin).get("result") or {}
if not isinstance(r,dict) or not r: sys.exit(0)
name="authenticated_transfer" if "authenticated_transfer" in r else sorted(r)[0]
words=r[name]
if not (isinstance(words,list) and len(words)==8): sys.exit(0)
print(name, "".join(struct.pack("<I", int(w)).hex() for w in words))')"
[ -n "${PROGRAM_ID:-}" ] && [ ${#PROGRAM_ID} -eq 64 ] || fail chain "getProgramIds gave no usable program id: $(echo "$PROGRAMS" | head -c 200)"
echo "      a program the chain knows: $PROGRAM_NAME = $PROGRAM_ID"

echo "[1/4] Building runtime + module + dependency modules..."
build_all "$WORK" "$ROOT"

echo "[2/4] Installing modules; starting the daemon; loading pilot; identity + funding..."
install_modules "$WORK" "$MODS"
start_daemon "$A_LC" "$MODS" "$A_LOG" PILOT_TCP_PORT=60000 PILOT_STORAGE_NAT=extip:127.0.0.1
load_pilot "$A_LC" "$A_LOG"
wait_funded "$A_LC" "$A_DATA" agent
wait_quiet "$A_LC" agent
echo "EVIDENCE role=programs agent_account=$ACCOUNT public_account=$PUB_B58 balance=$PBAL"

echo "[3/4] program.query on $PROGRAM_ID (a chain read through the wallet module)..."
Q=$(call "$A_LC" programQuery "$PROGRAM_ID" "{}")
echo "$Q" > "$OUT/query.json"
[ -z "$(echo "$Q" | field error)" ] || fail query "program.query answered an error: $Q"
case "$(echo "$Q" | field exists)" in True|true) ;; *) fail query "program.query says the program account does not exist: $Q";; esac
echo "      program account: balance $(echo "$Q" | field balance), nonce $(echo "$Q" | field nonce), owner $(echo "$Q" | field account.program_owner | head -c 16)…, data $(echo "$Q" | field account.data | wc -c) chars"
echo "EVIDENCE role=programs step=query program=$PROGRAM_ID exists=true balance=$(echo "$Q" | field balance) nonce=$(echo "$Q" | field nonce)"

# program.call refuses to guess: no instruction words, no accounts -> usage, not a transaction.
C=$(call "$A_LC" programCall "$PROGRAM_ID" "" "{}")
echo "$C" | grep -q "instruction must be\|params.accounts" || fail call "program.call with nothing to send did not refuse: $C"
echo "      program.call with no instruction/accounts is refused before the wallet is asked (as it must be)"

echo "[4/4] program.deploy builtin:token — the wallet module's built-in token program ELF, as a deployment transaction..."
D=$(call "$A_LC" programDeploy builtin:token)
# The daemon abandons the call at ~20 s while the wallet keeps working; ask again for the answer.
if [ -z "$D" ]; then sleep 20; D=$(call "$A_LC" programDeploy builtin:token); fi
echo "$D" > "$OUT/deploy.json"
echo "      wallet reply: $(echo "$D" | head -c 400)"
[ -n "$D" ] || fail deploy "program.deploy returned nothing (daemon window) twice"
[ -n "$(echo "$D" | field binary_hash)" ] || fail deploy "program.deploy did not reach the wallet (no binary hash in the reply): $D"
DEPLOYED=$(echo "$D" | field deployed); TXH=$(echo "$D" | field tx_hash); DERR=$(echo "$D" | field error)
if [ "$DEPLOYED" = "True" ] || [ "$DEPLOYED" = "true" ]; then
  [ ${#TXH} -eq 64 ] || fail deploy "wallet says deployed but gave no 64-hex tx hash: $D"
  BLK=""; for i in $(seq 1 20); do BLK=$(tx_block "$TXH"); [ -n "$BLK" ] && break; sleep 15; done
  [ -n "$BLK" ] || fail deploy "getTransaction($TXH) is still unknown to the chain after 5 min"
  echo "      ON CHAIN: deployment tx $TXH in block $BLK (size $(echo "$D" | field size_bytes) bytes, sha256 $(echo "$D" | field binary_hash | head -c 16)…)"
  echo "EVIDENCE role=programs step=deploy program=builtin:token deployed=yes tx=$TXH block=$BLK sha256=$(echo "$D" | field binary_hash)"
else
  echo "      the wallet did not deploy it; its reason, verbatim: ${DERR:-<none given>}"
  echo "EVIDENCE role=programs step=deploy program=builtin:token deployed=no wallet_error=\"${DERR:-<none>}\" sha256=$(echo "$D" | field binary_hash)"
fi
echo
echo "=== PROGRAM OPERATIONS PASSED in $(elapsed): program.query read $PROGRAM_ID through the wallet module; program.call refused to guess; program.deploy reached the wallet (deployed=$DEPLOYED${TXH:+, tx $TXH}) ==="
