#!/usr/bin/env bash
# LP-0008 — the owner talks to the agent from a SEPARATE program over Logos Messaging, with no
# server in between and no local connection to the agent's daemon.
#
#   agents/owner-channel.sh
#
# One agent (a fresh identity, funded from the faucet on the public testnet) and the owner client
# `pilot-owner` (pilot-owner/) share nothing but a Waku relay. The client seals and signs the
# agent's own envelope (pilot_crypto.cpp compiled in) and publishes it through the relay's REST
# API; the agent pulls its owner topic from the relay store, verifies the signature and nonce,
# EXECUTES the command and answers on the same topic, sealed to the owner's key; the client reads
# the reply back from the store. Three exchanges, every step asserted:
#   1. /balance                 -> the agent's balances come back to the client
#   2. /send <to> 101 …         -> above the 100-LEZ per-transaction limit: HELD, the agent tells
#                                  the owner and asks for /approve <id>
#   3. /approve <id>            -> the spend executes; the transaction is read back FROM THE CHAIN
# Evidence lines start with "EVIDENCE"; agents/out/owner/ keeps the logs, the card and the client's
# transcript. Env: see agents/lib.sh (LOGOSCORE_BIN/LGPM_BIN, LEZ_RPC, …); OWNER_BIN = a pre-built
# pilot-owner (else built here with nix).

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib.sh
. "$ROOT/agents/lib.sh"
need nix; need python3; need curl

OUT="$ROOT/agents/out/owner"; mkdir -p "$OUT"
rm -f "$OUT"/agent-daemon.log "$OUT"/agent-card.json "$OUT"/owner-transcript.txt
WORK="$(mktemp -d)"
MODS="$WORK/modules"; mkdir -p "$MODS"
A_LC="$WORK/a/lc"; A_DATA="$WORK/a/data"; A_LOG="$OUT/agent-daemon.log"
FAIL_LOGS="$A_LOG"
export PILOT_OWNER_HOME="$WORK/owner-home"       # the client's state: keys, pairing, nonce
DEMO_RECIPIENT_HEX="${DEMO_RECIPIENT_HEX:-f8fc394c0e5440c4188236d1693076b0cfad04984cf67ca64e0e43a173144f63}"
HOLD_AMOUNT=101                                    # above the default 100-LEZ per-transaction limit

cleanup() { stop_daemon "$A_LC"; [ "${KEEP_RELAY:-0}" = "1" ] || stop_relay; rm -rf "$WORK"; }
trap cleanup EXIT

echo "=== LP-0008 Pilot — owner channel from a separate app  (endpoint $LEZ_RPC) ==="
chain_check

echo "[1/6] Building runtime + module + dependency modules, and the owner client..."
build_all "$WORK" "$ROOT"
if [ -n "${OWNER_BIN:-}" ]; then OWNER="$OWNER_BIN"; else
  nix build "$ROOT/pilot-owner" -o "$WORK/r-owner" || fail build "pilot-owner (owner client)"
  OWNER="$WORK/r-owner/bin/pilot-owner"
fi
"$OWNER" selftest || fail build "pilot-owner selftest (envelope + ECIES round trip)"
echo "      owner client: $OWNER"

echo "[2/6] Starting the local Waku relay the agent and the client both use..."
start_relay
RELAY_REST="$PILOT_WAKU_REST"

echo "[3/6] Installing modules; starting the agent's daemon; loading pilot..."
install_modules "$WORK" "$MODS"
start_daemon "$A_LC" "$MODS" "$A_LOG" PILOT_TCP_PORT=60000 PILOT_STORAGE_NAT=extip:127.0.0.1
load_pilot "$A_LC" "$A_LOG"
COUNT=$(call "$A_LC" metaSkills | field count)
[ "$COUNT" = "23" ] || fail skills "expected 23 registered skills, got '$COUNT'"
echo "      daemon up; 23 skills"

echo "[4/6] Identity + self-funding from the faucet (the /approve below spends real testnet LEZ)..."
wait_funded "$A_LC" "$A_DATA" agent
wait_quiet "$A_LC" agent
A_PUB="$PUB"; A_PUB_B58="$PUB_B58"; A_PBAL="$PBAL"; A_ACCOUNT="$ACCOUNT"
call "$A_LC" agentCard > "$OUT/agent-card.json"
[ -n "$(field _logos.signing_key < "$OUT/agent-card.json")" ] || fail card "agentCard has no _logos.signing_key: $(head -c 200 "$OUT/agent-card.json")"
echo "EVIDENCE role=owner agent_account=$A_ACCOUNT public_account=$A_PUB_B58 balance=$A_PBAL"

echo "[5/6] Pairing: the owner makes a key, the agent is bound to it, the channel is established; then /balance..."
INIT=$("$OWNER" init); echo "$INIT" | sed 's/^/      owner: /'
OWNER_PUB=$(echo "$INIT" | sed -n 's/^owner public key: //p')
[ ${#OWNER_PUB} -ge 66 ] || fail owner "pilot-owner init printed no key"
R=$(call "$A_LC" metaConfigure owner.npk "$OWNER_PUB"); case "$R" in true|True|1) ;; *) fail bind "metaConfigure owner.npk answered '$R'";; esac
R=$(call "$A_LC" establishOwnerChannel); case "$R" in true|True|1) ;; *) fail bind "establishOwnerChannel answered '$R'";; esac
TOPIC=$(call "$A_LC" getOwnerChannelId)
[ "$TOPIC" = "/pilot/1/owner-$A_ACCOUNT/proto" ] || fail bind "owner topic is '$TOPIC', expected /pilot/1/owner-$A_ACCOUNT/proto"
"$OWNER" pair "$OUT/agent-card.json" "$A_ACCOUNT" --relay "$RELAY_REST" | sed 's/^/      owner: /'
echo "EVIDENCE role=owner step=pair owner_key=$OWNER_PUB topic=$TOPIC relay=$RELAY_REST"

# exchange <label> <text> <pattern> [tries]: the client sends; the agent PULLS its owner topic
# from the relay store (agentPoll), verifies + executes + answers; the client reads the store.
# Prints the client's transcript so far; fails if the pattern never appears.
exchange() {
  local LABEL="$1" TEXT="$2" PATTERN="$3" TRIES="${4:-30}" SENT REPLY
  SENT=$("$OWNER" send "$TEXT") || fail send "[$LABEL] pilot-owner send failed: $SENT"
  echo "      owner -> agent: $TEXT   ($SENT)"
  for i in $(seq 1 "$TRIES"); do
    call "$A_LC" agentPoll >/dev/null
    REPLY=$("$OWNER" listen --since 900)
    if echo "$REPLY" | grep -qE "$PATTERN"; then
      echo "$REPLY" | grep -E "$PATTERN" | head -3 | sed 's/^/      agent -> owner: /'
      echo "$REPLY" >> "$OUT/owner-transcript.txt"
      return 0
    fi
    sleep 5
  done
  echo "$REPLY" >> "$OUT/owner-transcript.txt"
  fail receive "[$LABEL] no reply matching /$PATTERN/ reached the owner client (last transcript: $(echo "$REPLY" | tail -3 | head -c 300))"
}

exchange balance "/balance" '"balance"'
echo "EVIDENCE role=owner step=command command=/balance reply=balances-received-by-client"

echo "[6/6] A held spend approved from the separate app: /send $HOLD_AMOUNT LEZ (above the limit) -> HELD -> /approve -> on chain..."
read -r RB0 RN0 <<<"$(acct "$DEMO_RECIPIENT_HEX")"
[ -n "$RB0" ] || fail spend "recipient $(b58 "$DEMO_RECIPIENT_HEX") is not a registered account on this chain"
exchange hold "/send public:$DEMO_RECIPIENT_HEX $HOLD_AMOUNT owner-channel approval test" '/approve [0-9a-f]+'
REQ_ID=$(grep -oE '/approve [0-9a-f]+' "$OUT/owner-transcript.txt" | tail -1 | awk '{print $2}')
[ -n "$REQ_ID" ] || fail hold "no request id in the agent's hold notice"
echo "      held request $REQ_ID (the agent asked the owner to approve)"
echo "EVIDENCE role=owner step=hold request=$REQ_ID amount=$HOLD_AMOUNT limit=100 state=HELD notified_owner=yes"
exchange approve "/approve $REQ_ID" "approved $REQ_ID"
TX=$(call "$A_LC" walletHistory | python3 -c 'import sys,json
req=sys.argv[1]
for t in json.loads(sys.stdin.read() or "{}").get("transactions",[]):
    if t.get("id")==req: print(t.get("tx_hash",""), t.get("state",""))' "$REQ_ID")
read -r TXH TXS <<<"$TX"
[ "$TXS" = "COMPLETED" ] && [ ${#TXH} -eq 64 ] || fail approve "spend $REQ_ID is '$TXS' with hash '$TXH' after the owner's approval"
BLK=""; for i in $(seq 1 20); do BLK=$(tx_block "$TXH"); [ -n "$BLK" ] && break; sleep 15; done
[ -n "$BLK" ] || fail chain "getTransaction($TXH) is still unknown to the chain after 5 min"
RB1=""; for i in $(seq 1 20); do read -r RB1 RN1 <<<"$(acct "$DEMO_RECIPIENT_HEX")"; [ "${RB1:-0}" -ge $(( RB0 + HOLD_AMOUNT )) ] && break; sleep 15; done
[ "${RB1:-0}" -ge $(( RB0 + HOLD_AMOUNT )) ] || fail chain "recipient balance did not rise by $HOLD_AMOUNT: $RB0 -> ${RB1:-?}"
read -r PBAL1 PNONCE1 <<<"$(acct "$A_PUB")"
echo "      ON CHAIN: tx $TXH in block $BLK; recipient $RB0 -> $RB1; agent public $A_PBAL -> $PBAL1  [$(elapsed)]"
echo "EVIDENCE role=owner step=approve request=$REQ_ID tx=$TXH block=$BLK recipient=\"$RB0 -> $RB1\" agent_public=\"$A_PBAL -> $PBAL1\" approved_via=pilot-owner-over-relay"
echo
echo "=== OWNER CHANNEL PASSED in $(elapsed): the owner client (a separate program, relay REST only) got the agent's balances, was told about a held $HOLD_AMOUNT-LEZ spend, approved it, and the spend settled on chain (tx $TXH, block $BLK) ==="
