#!/usr/bin/env bash
# LP-0008 — the owner talks to the agent from SEPARATE programs over Logos Messaging, with no
# server in between and no local connection to the agent's daemon.
#
#   agents/owner-channel.sh
#
# One agent (a fresh identity, funded from the faucet on the public testnet) and two owner
# front-ends that share nothing with it but a Waku relay:
#
#   A. the pilot_owner MODULE (pilot-owner/module) — what the Basecamp plugin Pilot Remote
#      (pilot-ui/remote-plugin) calls for every key or relay operation. Basecamp needs a display,
#      so the module is driven here exactly the way Basecamp drives it: loaded in a second
#      logoscore daemon and called by name (createKey, pair, send, poll). The agent is bound to
#      the key this module makes.
#   B. the pilot-owner CONSOLE client (pilot-owner/src/main.cpp), importing the same owner key —
#      the owner's second device.
#
# Both are built from one shared library (pilot-owner/src/owner_client.*) with the agent's own
# crypto compiled in: they sign and seal the agent's envelope, publish through the relay's REST
# API, and read the agent's sealed replies back from the relay store. The agent pulls its owner
# topic from the same store, verifies signature + nonce, EXECUTES the command and answers.
#
# Exchanges, every step asserted:
#   A1. createKey; the agent is bound to it; pair with the agent's card  (the Basecamp set-up screen)
#   A2. /balance                 -> the agent's balances reach the module
#   A3. /send <to> 101 …         -> above the 100-LEZ per-transaction limit: HELD, the agent tells
#                                  the owner and asks for /approve <id>
#   A4. /approve <id>            -> the spend executes; the transaction is read back FROM THE CHAIN
#   B1. import the key; pair; /balance -> the balances reach the console client too
#   B2. /send <to> 20 …          -> under the limit: executes at once; read back FROM THE CHAIN
#
# Evidence lines start with "EVIDENCE"; agents/out/owner/ keeps the logs, the card and both
# transcripts. Env: see agents/lib.sh (LOGOSCORE_BIN/LGPM_BIN, LEZ_RPC, …); OWNER_BIN = a
# pre-built pilot-owner, OWNER_MODULE_LGX = a pre-built pilot_owner .lgx (else built here with nix).

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib.sh
. "$ROOT/agents/lib.sh"
need nix; need python3; need curl

OUT="$ROOT/agents/out/owner"; mkdir -p "$OUT"
rm -f "$OUT"/agent-daemon.log "$OUT"/owner-module-daemon.log "$OUT"/agent-card.json \
      "$OUT"/module-transcript.txt "$OUT"/owner-transcript.txt "$OUT"/owner-backlog.txt
WORK="$(mktemp -d)"
MODS="$WORK/modules"; mkdir -p "$MODS"
A_LC="$WORK/a/lc"; A_DATA="$WORK/a/data"; A_LOG="$OUT/agent-daemon.log"
U_LC="$WORK/u/lc"; U_MODS="$WORK/u/modules"; U_LOG="$OUT/owner-module-daemon.log"; mkdir -p "$U_MODS"
U_HOME="$WORK/owner-module-home"                    # the module's state: the owner's key, pairing, nonce
FAIL_LOGS="$A_LOG $U_LOG"
export PILOT_OWNER_HOME="$WORK/owner-console-home"  # the console client's state (imports the same key)
DEMO_RECIPIENT_HEX="${DEMO_RECIPIENT_HEX:-f8fc394c0e5440c4188236d1693076b0cfad04984cf67ca64e0e43a173144f63}"
HOLD_AMOUNT=101                                     # above the default 100-LEZ per-transaction limit
SMALL_AMOUNT=20                                     # under it: executes without a hold

cleanup() { stop_daemon "$U_LC"; stop_daemon "$A_LC"; [ "${KEEP_RELAY:-0}" = "1" ] || stop_relay; rm -rf "$WORK"; }
trap cleanup EXIT

echo "=== LP-0008 Pilot — owner channel from separate apps  (endpoint $LEZ_RPC) ==="
chain_check

echo "[1/8] Building runtime + module + dependency modules, the owner module and the owner console client..."
build_all "$WORK" "$ROOT"
if [ -n "${OWNER_MODULE_LGX:-}" ]; then OWNER_LGX="$OWNER_MODULE_LGX"; else
  nix build "$ROOT/pilot-owner/module#lgx" -o "$WORK/r-owner-module" || fail build "pilot_owner module (pilot-owner/module#lgx)"
  OWNER_LGX="$(ls "$WORK"/r-owner-module/*.lgx 2>/dev/null | head -1)"
fi
[ -n "$OWNER_LGX" ] && [ -f "$OWNER_LGX" ] || fail build "no .lgx package for the pilot_owner module"
if [ -n "${OWNER_BIN:-}" ]; then OWNER="$OWNER_BIN"; else
  nix build "$ROOT/pilot-owner" -o "$WORK/r-owner" || fail build "pilot-owner (owner console client)"
  OWNER="$WORK/r-owner/bin/pilot-owner"
fi
"$OWNER" selftest || fail build "pilot-owner selftest (the shared library's pure pieces)"
echo "      owner module: $OWNER_LGX"
echo "      owner client: $OWNER"

echo "[2/8] Starting the local Waku relay the agent and both owner front-ends use..."
start_relay
RELAY_REST="$PILOT_WAKU_REST"

echo "[3/8] Installing modules; starting the agent's daemon; loading pilot..."
install_modules "$WORK" "$MODS"
start_daemon "$A_LC" "$MODS" "$A_LOG" PILOT_TCP_PORT=60000 PILOT_STORAGE_NAT=extip:127.0.0.1
load_pilot "$A_LC" "$A_LOG"
COUNT=$(call "$A_LC" metaSkills | field count)
[ "$COUNT" = "23" ] || fail skills "expected 23 registered skills, got '$COUNT'"
echo "      daemon up; 23 skills"

echo "[4/8] Identity + self-funding from the faucet (the spends below move real testnet LEZ)..."
wait_funded "$A_LC" "$A_DATA" agent
wait_quiet "$A_LC" agent
A_PUB="$PUB"; A_PUB_B58="$PUB_B58"; A_PBAL="$PBAL"; A_ACCOUNT="$ACCOUNT"
call "$A_LC" agentCard > "$OUT/agent-card.json"
[ -n "$(field _logos.signing_key < "$OUT/agent-card.json")" ] || fail card "agentCard has no _logos.signing_key: $(head -c 200 "$OUT/agent-card.json")"
echo "EVIDENCE role=owner agent_account=$A_ACCOUNT public_account=$A_PUB_B58 balance=$A_PBAL"

echo "[5/8] The owner MODULE (Basecamp's path) in its own daemon: make the key, bind the agent to it, pair..."
"$LGPM" install --file "$OWNER_LGX" --modules-dir "$U_MODS" --allow-unsigned >/dev/null 2>&1 || fail install "$OWNER_LGX"
start_daemon "$U_LC" "$U_MODS" "$U_LOG" PILOT_OWNER_HOME="$U_HOME"
load_only "$U_LC" pilot_owner "$U_LOG"
E=$(callm "$U_LC" pilot_owner echo hello)
[ "$E" = "echo: hello" ] || fail load "pilot_owner echo answered '$E'"
K=$(callm "$U_LC" pilot_owner createKey)
[ "$(echo "$K" | field ok)" = "True" ] || fail owner "createKey answered: $K"
OWNER_PUB=$(echo "$K" | field owner_pub)
[ ${#OWNER_PUB} -ge 66 ] || fail owner "createKey returned no key: $K"
echo "      module: owner key made — $(echo "$K" | field bind)"
R=$(call "$A_LC" metaConfigure owner.npk "$OWNER_PUB"); case "$R" in true|True|1) ;; *) fail bind "metaConfigure owner.npk answered '$R'";; esac
R=$(call "$A_LC" establishOwnerChannel); case "$R" in true|True|1) ;; *) fail bind "establishOwnerChannel answered '$R'";; esac
TOPIC=$(call "$A_LC" getOwnerChannelId)
[ "$TOPIC" = "/pilot/1/owner-$A_ACCOUNT/proto" ] || fail bind "owner topic is '$TOPIC', expected /pilot/1/owner-$A_ACCOUNT/proto"
P=$(callm "$U_LC" pilot_owner pair "$OUT/agent-card.json" "$A_ACCOUNT" "$RELAY_REST")
[ "$(echo "$P" | field ok)" = "True" ] || fail pair "module pair answered: $P"
[ "$(echo "$P" | field topic)" = "$TOPIC" ] || fail pair "module pair computed topic '$(echo "$P" | field topic)', the agent listens on $TOPIC"
S=$(callm "$U_LC" pilot_owner status)
[ "$(echo "$S" | field paired)" = "True" ] || fail pair "module status is not paired: $S"
echo "      module: paired with $(echo "$P" | field agent_name) on $TOPIC via $RELAY_REST"
echo "EVIDENCE role=owner step=pair front_end=pilot_owner-module owner_key=$OWNER_PUB topic=$TOPIC relay=$RELAY_REST"

# mexchange <label> <text> <pattern> [tries]: the MODULE sends; the agent pulls its owner topic
# from the relay store (agentPoll), verifies + executes + answers; the module polls the store.
# Every reply goes to module-transcript.txt; fails if the pattern never appears.
mexchange() {
  local LABEL="$1" TEXT="$2" PATTERN="$3" TRIES="${4:-30}" SENT GOT NEW
  SENT=$(callm "$U_LC" pilot_owner send "$TEXT")
  [ "$(echo "$SENT" | field ok)" = "True" ] || fail send "[$LABEL] module send failed: $SENT"
  echo "      module -> agent: $TEXT   (nonce $(echo "$SENT" | field nonce), $(echo "$SENT" | field sealed_bytes) bytes sealed)"
  for i in $(seq 1 "$TRIES"); do
    call "$A_LC" agentPoll >/dev/null
    GOT=$(callm "$U_LC" pilot_owner poll)
    [ "$(echo "$GOT" | field ok)" = "True" ] || echo "      (module poll: $(echo "$GOT" | field error))"
    NEW=$(echo "$GOT" | python3 -c 'import sys,json
try: d=json.loads(sys.stdin.read() or "{}")
except Exception: d={}
for r in d.get("replies",[]): print(r.get("text",""))')
    [ -n "$NEW" ] && echo "$NEW" >> "$OUT/module-transcript.txt"
    if grep -qE "$PATTERN" "$OUT/module-transcript.txt" 2>/dev/null; then
      echo "      agent -> module: $(grep -E "$PATTERN" "$OUT/module-transcript.txt" | tail -1 | head -c 300)"
      return 0
    fi
    sleep 5
  done
  fail receive "[$LABEL] no reply matching /$PATTERN/ reached the owner module (transcript tail: $(tail -3 "$OUT/module-transcript.txt" 2>/dev/null | head -c 300))"
}

echo "[6/8] Through the module: /balance, then a held $HOLD_AMOUNT-LEZ spend approved from it and settled on chain..."
mexchange balance "/balance" '"balance"'
echo "EVIDENCE role=owner step=command front_end=pilot_owner-module command=/balance reply=balances-received"
read -r RB0 RN0 <<<"$(acct "$DEMO_RECIPIENT_HEX")"
[ -n "$RB0" ] || fail spend "recipient $(b58 "$DEMO_RECIPIENT_HEX") is not a registered account on this chain"
mexchange hold "/send public:$DEMO_RECIPIENT_HEX $HOLD_AMOUNT owner-module approval test" '/approve [0-9a-f]+'
REQ_ID=$(grep -oE '/approve [0-9a-f]+' "$OUT/module-transcript.txt" | tail -1 | awk '{print $2}')
[ -n "$REQ_ID" ] || fail hold "no request id in the agent's hold notice"
echo "      held request $REQ_ID (the agent asked the owner to approve)"
echo "EVIDENCE role=owner step=hold front_end=pilot_owner-module request=$REQ_ID amount=$HOLD_AMOUNT limit=100 state=HELD notified_owner=yes"
mexchange approve "/approve $REQ_ID" "approved $REQ_ID"
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
echo "EVIDENCE role=owner step=approve front_end=pilot_owner-module request=$REQ_ID tx=$TXH block=$BLK recipient=\"$RB0 -> $RB1\" agent_public=\"$A_PBAL -> $PBAL1\" approved_via=pilot_owner-module-over-relay"

echo "[7/8] The CONSOLE client, importing the same owner key (the owner's second device): /balance, then a $SMALL_AMOUNT-LEZ spend under the limit..."
KEYPAIR=$(python3 -c 'import sys,json
d=json.load(open(sys.argv[1])); print(d["owner_priv"]+":"+d["owner_pub"])' "$U_HOME/state.json")
INIT=$("$OWNER" init --import "$KEYPAIR") || fail owner "pilot-owner init --import failed: $INIT"
echo "$INIT" | sed 's/^/      owner: /'
[ "$(echo "$INIT" | sed -n 's/^owner public key: //p')" = "$OWNER_PUB" ] || fail owner "the console client imported a different key"
"$OWNER" pair "$OUT/agent-card.json" "$A_ACCOUNT" --relay "$RELAY_REST" | sed 's/^/      owner: /'
# The module leg's replies are sealed to this same key: read them once now so they count as seen
# and only NEW replies reach the exchanges below.
"$OWNER" listen --since 900 > "$OUT/owner-backlog.txt" 2>&1
echo "      console: $(grep -c "agent:" "$OUT/owner-backlog.txt" 2>/dev/null) earlier replies read and set aside"
echo "EVIDENCE role=owner step=pair front_end=pilot-owner-console owner_key=$OWNER_PUB topic=$TOPIC relay=$RELAY_REST"

# exchange <label> <text> <pattern> [tries]: the CONSOLE client sends and reads the store.
exchange() {
  local LABEL="$1" TEXT="$2" PATTERN="$3" TRIES="${4:-30}" SENT REPLY
  SENT=$("$OWNER" send "$TEXT") || fail send "[$LABEL] pilot-owner send failed: $SENT"
  echo "      owner -> agent: $TEXT   ($SENT)"
  for i in $(seq 1 "$TRIES"); do
    call "$A_LC" agentPoll >/dev/null
    REPLY=$("$OWNER" listen --since 900)
    echo "$REPLY" | grep -v '^(no reply' >> "$OUT/owner-transcript.txt"
    if grep -qE "$PATTERN" "$OUT/owner-transcript.txt" 2>/dev/null; then
      echo "      agent -> owner: $(grep -E "$PATTERN" "$OUT/owner-transcript.txt" | tail -1 | head -c 300)"
      return 0
    fi
    sleep 5
  done
  fail receive "[$LABEL] no reply matching /$PATTERN/ reached the owner client (transcript tail: $(tail -3 "$OUT/owner-transcript.txt" 2>/dev/null | head -c 300))"
}

exchange balance "/balance" '"balance"'
echo "EVIDENCE role=owner step=command front_end=pilot-owner-console command=/balance reply=balances-received"
read -r RB2 RN2 <<<"$(acct "$DEMO_RECIPIENT_HEX")"
exchange small "/send public:$DEMO_RECIPIENT_HEX $SMALL_AMOUNT owner-console test" '"status":"completed"'
TXH2=$(grep -oE '"tx_hash":"[0-9a-f]{64}"' "$OUT/owner-transcript.txt" | tail -1 | cut -d'"' -f4)
[ ${#TXH2} -eq 64 ] || fail small "the agent's reply to the $SMALL_AMOUNT-LEZ send carries no tx hash"
BLK2=""; for i in $(seq 1 20); do BLK2=$(tx_block "$TXH2"); [ -n "$BLK2" ] && break; sleep 15; done
[ -n "$BLK2" ] || fail chain "getTransaction($TXH2) is still unknown to the chain after 5 min"
RB3=""; for i in $(seq 1 20); do read -r RB3 RN3 <<<"$(acct "$DEMO_RECIPIENT_HEX")"; [ "${RB3:-0}" -ge $(( RB2 + SMALL_AMOUNT )) ] && break; sleep 15; done
[ "${RB3:-0}" -ge $(( RB2 + SMALL_AMOUNT )) ] || fail chain "recipient balance did not rise by $SMALL_AMOUNT: $RB2 -> ${RB3:-?}"
read -r PBAL2 PNONCE2 <<<"$(acct "$A_PUB")"
echo "      ON CHAIN: tx $TXH2 in block $BLK2; recipient $RB2 -> $RB3; agent public $PBAL1 -> $PBAL2  [$(elapsed)]"
echo "EVIDENCE role=owner step=send front_end=pilot-owner-console amount=$SMALL_AMOUNT tx=$TXH2 block=$BLK2 recipient=\"$RB2 -> $RB3\" agent_public=\"$PBAL1 -> $PBAL2\" sent_via=pilot-owner-console-over-relay"

echo "[8/8] Done."
echo
echo "=== OWNER CHANNEL PASSED in $(elapsed): the owner module (Basecamp's path) made the key, paired, got the balances, was told about a held $HOLD_AMOUNT-LEZ spend and approved it (tx $TXH, block $BLK); the console client imported the same key, got the balances and sent $SMALL_AMOUNT LEZ (tx $TXH2, block $BLK2) — separate programs, relay REST only ==="
