#!/usr/bin/env bash
# LP-0008 Pilot — deploy ONE agent per default skill category on the PUBLIC LEZ testnet and prove
# the category's skills end to end, from a clean clone, with evidence a reviewer can re-check.
#
#   agents/deploy-agent.sh --role blockchain    # ./demo.sh: identity, faucet, a spend read back from the chain
#   agents/deploy-agent.sh --role storage       # A uploads + shares a file; B (a second identity) fetches
#                                               # it from A's node over the storage network, decrypts it,
#                                               # byte-identical
#   agents/deploy-agent.sh --role messaging     # A -> B direct message; A creates a group, invites B;
#                                               # B joins; a message each way in the group — all READ BACK
#                                               # on the receiving side (messagingInbox), not just sent
#
# Two of the prize's illustrative USE CASES run on the same two-agent scaffold (docs/use-cases.md):
#   agents/deploy-agent.sh --role alerter       # on-chain event alerter: A watches B's public account through
#                                               # the wallet module; B spends; A sees the balance change and
#                                               # alerts B over Logos Messaging; B's inbox shows the alert
#   agents/deploy-agent.sh --role marketplace   # paid skill marketplace, REAL proofs (RISC0_DEV_MODE=0, ~16 GB):
#                                               # A's private account is funded by a real shielded proof; B sells
#                                               # agent.ask (needs DEEPSEEK_API_KEY); A discovers B's card, buys
#                                               # the task, and pays B's declared price over the private rail —
#                                               # settled on chain, balances read back
#
# storage and messaging run TWO daemons in one job (the role agent A and its counterparty B), each
# with its own identity, data dir, config dir and ports, both funded from the faucet, plus one local
# nwaku relay the share / message payloads travel through (the pull path reads its REST store).
# Every step is asserted; the script exits 1 the moment something does not do what it claims and
# prints the daemon log tails. Evidence lines start with "EVIDENCE" and the whole run's artifacts
# (logs, inbox dumps, the file and its downloaded copy) are left in agents/out/<role>/.
#
# Env: see agents/lib.sh (LOGOSCORE_BIN/LGPM_BIN, LEZ_RPC, FUND_TIMEOUT_SECS, RISC0_DEV_MODE).

set -uo pipefail
ROLE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --role) ROLE="${2:-}"; shift 2;;
    --role=*) ROLE="${1#--role=}"; shift;;
    *) echo "unknown argument: $1"; exit 2;;
  esac
done
case "$ROLE" in storage|messaging|blockchain|alerter|marketplace) ;; *) echo "usage: $0 --role storage|messaging|blockchain|alerter|marketplace"; exit 2;; esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ "$ROLE" = "blockchain" ]; then
  # The Blockchain agent is what the demo deploys: a fresh identity, faucet funding, a spend
  # through the spending FSM, all read back from the chain. Same script a reviewer runs.
  echo "=== agent role: blockchain -> ./demo.sh ==="
  exec "$ROOT/demo.sh"
fi

# shellcheck source=lib.sh
. "$ROOT/agents/lib.sh"
need nix; need python3; need curl; need cmp

# Never rm -rf $OUT here: the caller may already be tee-ing run.log into it (the workflow does),
# and unlinking that file cost the first CI run its run log. Clear only what this script writes.
OUT="$ROOT/agents/out/$ROLE"; mkdir -p "$OUT"
rm -f "$OUT"/agent-*-daemon.log "$OUT"/agent-*-storage.log "$OUT"/agent-*-inbox.json "$OUT"/last-inbox.json \
      "$OUT"/shared-original.txt "$OUT"/shared-downloaded-by-b.txt
WORK="$(mktemp -d)"
MODS="$WORK/modules"; mkdir -p "$MODS"
A_LC="$WORK/a/lc"; A_DATA="$WORK/a/data"; A_LOG="$OUT/agent-a-daemon.log"
B_LC="$WORK/b/lc"; B_DATA="$WORK/b/data"; B_LOG="$OUT/agent-b-daemon.log"
FAIL_LOGS="$A_LOG $B_LOG"

cleanup() {
  stop_daemon "$A_LC"; stop_daemon "$B_LC"
  cp "$A_DATA/storage/storage.log" "$OUT/agent-a-storage.log" 2>/dev/null || true
  cp "$B_DATA/storage/storage.log" "$OUT/agent-b-storage.log" 2>/dev/null || true
  [ "${KEEP_RELAY:-0}" = "1" ] || stop_relay
  rm -rf "$WORK"
}
trap cleanup EXIT

# Per-daemon ports so two agents fit on one host. The storage node's LISTEN port is fixed (and
# its NAT set to the loopback IP) so A has an address B can dial: /ip4/127.0.0.1/tcp/<port>.
A_ENV=(PILOT_TCP_PORT=60000 PILOT_STORAGE_API_PORT=5988 PILOT_STORAGE_DISC_PORT=8090 PILOT_STORAGE_LISTEN_PORT=8070 PILOT_STORAGE_NAT=extip:127.0.0.1)
B_ENV=(PILOT_TCP_PORT=60001 PILOT_STORAGE_API_PORT=5989 PILOT_STORAGE_DISC_PORT=8091 PILOT_STORAGE_LISTEN_PORT=8071 PILOT_STORAGE_NAT=extip:127.0.0.1)
A_STORAGE_ADDR="/ip4/127.0.0.1/tcp/8070"
DEMO_RECIPIENT_HEX="${DEMO_RECIPIENT_HEX:-f8fc394c0e5440c4188236d1693076b0cfad04984cf67ca64e0e43a173144f63}"   # a registered project account
if [ "$ROLE" = "marketplace" ]; then
  # The payer's payment rides the PRIVATE rail (the doer's card pays out to its private keys), and the
  # public testnet mines only real proofs: A proves for real — its shielded funding leg inside
  # initialize (~50 min on a 16 GB runner) and then the payment itself. B only RECEIVES, so B proves in
  # dev mode (its own shielded leg is accepted into the mempool and never mined; B's public account is
  # still credited, which is all B needs). Two real provers at once would not fit in 16 GB.
  [ -n "${DEEPSEEK_API_KEY:-}" ] || fail prereq "DEEPSEEK_API_KEY is not set — the doer needs a language model to sell agent.ask"
  A_ENV+=(RISC0_DEV_MODE=0 RISC0_INFO=1 "RISC0_SEGMENT_PO2=${RISC0_SEGMENT_PO2:-18}" "PILOT_TX_TIMEOUT_MS=${PILOT_TX_TIMEOUT_MS_PROOF:-14400000}")
  B_ENV+=(RISC0_DEV_MODE=1)
  R0VM_DIR="$(find "$HOME/.risc0/extensions" -maxdepth 1 -type d -name '*cargo-risczero*' 2>/dev/null | head -1)"
  [ -n "$R0VM_DIR" ] && export PATH="$R0VM_DIR:$PATH"
  command -v r0vm >/dev/null 2>&1 || fail prereq "r0vm (the RISC0 prover) is not on PATH — real proofs need it"
fi

echo "=== LP-0008 Pilot — agent role: $ROLE  (endpoint $LEZ_RPC, RISC0_DEV_MODE=$RISC0_DEV_MODE) ==="
chain_check

echo "[1/6] Building runtime + module + dependency modules (nix; pinned revisions)..."
build_all "$WORK" "$ROOT"

echo "[2/6] Starting the local Waku relay both agents dial..."
start_relay

echo "[3/6] Installing modules; starting two daemons (A = the $ROLE agent, B = its counterparty); loading pilot in each..."
install_modules "$WORK" "$MODS"
start_daemon "$A_LC" "$MODS" "$A_LOG" "${A_ENV[@]}"
start_daemon "$B_LC" "$MODS" "$B_LOG" "${B_ENV[@]}"
load_pilot "$A_LC" "$A_LOG"; load_pilot "$B_LC" "$B_LOG"
for who in A B; do
  LCD=$([ "$who" = A ] && echo "$A_LC" || echo "$B_LC")
  COUNT=$(call "$LCD" metaSkills | field count)
  [ "$COUNT" = "23" ] || fail skills "[$who] expected 23 registered skills, got '$COUNT'"
done
echo "      both daemons up; pilot + lez_core + delivery_module + storage_module loaded; 23 skills each"

echo "[4/6] Two identities, each funding ITSELF from the faucet (chain replay first; polls counted, not wall-clock)..."
# One after the other, not side by side. The faucet's puzzle data changes with every claim it
# pays, so two agents claiming in the same minute race each other: the loser's solution is
# stale, its claim is accepted and never credited (runs 33923468614 / 33925547986). The module
# now re-reads the faucet and retries; funding A before B removes the race we cause ourselves.
wait_funded "$A_LC" "$A_DATA" A; A_PUB="$PUB"; A_PUB_B58="$PUB_B58"; A_PBAL="$PBAL"; A_PNONCE="$PNONCE"; A_ACCOUNT="$ACCOUNT"
wait_funded "$B_LC" "$B_DATA" B; B_PUB="$PUB"; B_PUB_B58="$PUB_B58"; B_PBAL="$PBAL"; B_PNONCE="$PNONCE"; B_ACCOUNT="$ACCOUNT"
wait_quiet "$A_LC" A; wait_quiet "$B_LC" B

# An agent is CLOSED for hire on boot and listens on no inbox — nobody can reach it. Receiving a
# message or a shared key means being reachable, so both open. Not scored on the call's reply
# (the first-time subscribes can outlast the daemon's RPC window); the state read is the truth.
call "$A_LC" agentOpenForHire >/dev/null; call "$B_LC" agentOpenForHire >/dev/null
for who in A B; do
  LCD=$([ "$who" = A ] && echo "$A_LC" || echo "$B_LC")
  OPEN=$(call "$LCD" agentIsOpenForHire); case "$OPEN" in true|True|1) ;; *) fail open "[$who] did not open for hire: '$OPEN'";; esac
done
# The inbox topic is named after the agent's ENCRYPTION key, the one its card advertises.
A_KEY=$(call "$A_LC" agentCard | field _logos.enc_key); B_KEY=$(call "$B_LC" agentCard | field _logos.enc_key)
[ ${#A_KEY} -ge 64 ] && [ ${#B_KEY} -ge 64 ] || fail keys "no encryption key in a card (A='${A_KEY:0:12}' B='${B_KEY:0:12}')"
echo "      A enc key ${A_KEY:0:16}…  B enc key ${B_KEY:0:16}…  (both open for hire)  [$(elapsed)]"
echo "EVIDENCE role=$ROLE agent=A account=$A_ACCOUNT public_account=$A_PUB_B58 balance=$A_PBAL nonce=$A_PNONCE enc_key=$A_KEY"
echo "EVIDENCE role=$ROLE agent=B account=$B_ACCOUNT public_account=$B_PUB_B58 balance=$B_PBAL nonce=$B_PNONCE enc_key=$B_KEY"

# poll_until <config-dir> <label> <python-predicate over the inbox JSON> [tries]
# The pull path: agentPoll reads the relay store, then messagingInbox is checked.
poll_inbox_until() {
  local LCD="$1" LABEL="$2" PRED="$3" TRIES="${4:-24}" INBOX
  for i in $(seq 1 "$TRIES"); do
    call "$LCD" agentPoll >/dev/null
    INBOX=$(call "$LCD" messagingInbox)
    if echo "$INBOX" | python3 -c "import sys,json
d=json.loads(sys.stdin.read() or '{}'); ms=d.get('messages',[])
sys.exit(0 if any($PRED) else 1)" 2>/dev/null; then echo "$INBOX"; return 0; fi
    sleep 5
  done
  echo "$INBOX" > "$OUT/last-inbox.json"
  fail receive "$LABEL never arrived (inbox after $TRIES polls: $(echo "$INBOX" | head -c 300))"
}

if [ "$ROLE" = "storage" ]; then
  echo "[5/6] storage: A uploads (encrypted), lists, shares the key with B over Logos Messaging..."
  # A's node identity BEFORE its first start (the typed peerId call answers pre-start; after
  # the start the host drops replies and storagePeerInfo falls back to the node's own log).
  A_PEER=$(call "$A_LC" storagePeerInfo | field peer_id)
  echo "vault contents for a second identity $(date -u +%FT%TZ) $RANDOM" > "$OUT/shared-original.txt"
  UP=$(call "$A_LC" storageUpload "$OUT/shared-original.txt" shared-doc)
  CID=$(echo "$UP" | field cid); [ -n "$CID" ] || fail upload "storage.upload returned no CID: $UP"
  if [ -z "$A_PEER" ]; then sleep 3; PI=$(call "$A_LC" storagePeerInfo); A_PEER=$(echo "$PI" | field peer_id); echo "      A peer info (post-start): $PI"; fi
  [ -n "$A_PEER" ] || fail peer "A's storage node has no peer id (storagePeerInfo, pre- and post-start)"
  LIST=$(call "$A_LC" storageList); echo "$LIST" | grep -q "$CID" || fail list "storage.list on A does not show $CID"
  SHARE=$(call "$A_LC" storageShare "$CID" "$B_KEY")
  [ "$(echo "$SHARE" | field shared)" = "True" ] || [ "$(echo "$SHARE" | field shared)" = "true" ] || fail share "storage.share answered: $SHARE"
  echo "      A: cid $CID uploaded; shared to B on $(echo "$SHARE" | field topic)  [$(elapsed)]"
  echo "EVIDENCE role=storage step=upload agent=A cid=$CID label=shared-doc"
  echo "EVIDENCE role=storage step=share agent=A cid=$CID to=B topic=$(echo "$SHARE" | field topic)"

  echo "[6/6] storage: B receives the key, dials A's node, fetches the CID over the storage network, decrypts..."
  INBOX=$(poll_inbox_until "$B_LC" "the share" "m.get('kind')=='file_share' and '$CID' in m.get('message','') for m in ms")
  echo "$INBOX" > "$OUT/agent-b-inbox.json"
  call "$B_LC" storageList | grep -q "$CID" || fail share "B's storage.list does not show the shared CID after receiving the key"
  echo "      B: share received — storage.list on B shows $CID (shared by A)"
  # A plain multiaddr, not a JSON array: the CLI must not get a chance to re-parse the argument.
  CONN=$(call "$B_LC" storageConnect "$A_PEER" "$A_STORAGE_ADDR")
  echo "      B: dialing A's storage node $A_PEER at $A_STORAGE_ADDR -> $CONN"
  sleep 5
  # The daemon abandons the call at ~20 s while the module keeps fetching; the FILE is the truth.
  DL_OUT="$OUT/shared-downloaded-by-b.txt"; rm -f "$DL_OUT"
  for attempt in 1 2 3; do
    DL=$(call "$B_LC" storageDownload "$CID" "$DL_OUT")
    for i in $(seq 1 24); do [ -s "$DL_OUT" ] && break; sleep 5; done
    [ -s "$DL_OUT" ] && break
    echo "      … attempt $attempt: no file yet (download answered: ${DL:-<abandoned by the daemon>}); retrying"
  done
  [ -s "$DL_OUT" ] || fail download "B never received $CID from A's node (see agent-b-storage.log)"
  cmp -s "$OUT/shared-original.txt" "$DL_OUT" || fail download "B's copy of $CID differs from A's original"
  echo "      B: $CID fetched from A's node and decrypted — byte-identical to A's original  [$(elapsed)]"
  echo "EVIDENCE role=storage step=download agent=B cid=$CID from_peer=$A_PEER identical=yes bytes=$(wc -c <"$DL_OUT")"
  echo
  echo "=== STORAGE AGENT PASSED in $(elapsed): two funded identities ($A_PUB_B58, $B_PUB_B58); A stored + shared $CID; B fetched it from A's node and decrypted it byte-identical ==="
  exit 0
fi

if [ "$ROLE" = "alerter" ]; then
  echo "[5/6] alerter: A watches B's public account on the chain (through the wallet module); B spends 1 LEZ..."
  # The account B's faucet claim credited: a fresh account on this chain, so any change is B's doing.
  R0=$(call "$A_LC" chainAccount "$B_PUB")
  [ "$(echo "$R0" | field exists)" = "True" ] || [ "$(echo "$R0" | field exists)" = "true" ] || fail watch "A cannot read B's public account $B_PUB_B58: $R0"
  BAL0=$(echo "$R0" | field balance); N0=$(echo "$R0" | field nonce)
  echo "      A reads $B_PUB_B58: balance $BAL0, nonce $N0 (chainAccount; raw account record: $(echo "$R0" | field account | head -c 240))"
  echo "EVIDENCE role=alerter step=watch watcher=A target=$B_PUB_B58 balance=$BAL0 nonce=$N0"
  # The event: B pays 1 LEZ to the project account through its own spending FSM (public rail).
  read -r RB0 RN0 <<<"$(acct "$DEMO_RECIPIENT_HEX")"
  [ -n "$RB0" ] || fail spend "recipient $(b58 "$DEMO_RECIPIENT_HEX") is not a registered account on this chain (set DEMO_RECIPIENT_HEX)"
  SEND=$(call "$B_LC" walletSend "public:$DEMO_RECIPIENT_HEX" 1 "alerter use case: the event the watcher must catch")
  [ "$(echo "$SEND" | field status)" = "completed" ] || fail spend "B's walletSend answered '$SEND'"
  REQ=$(echo "$SEND" | field request_id)
  TX=$(call "$B_LC" walletHistory | python3 -c 'import sys,json
req=sys.argv[1]
for t in json.loads(sys.stdin.read() or "{}").get("transactions",[]):
    if t.get("id")==req: print(t.get("tx_hash",""), t.get("state",""))' "$REQ")
  read -r TXH TXS <<<"$TX"
  [ "$TXS" = "COMPLETED" ] && [ ${#TXH} -eq 64 ] || fail spend "B's spend row $REQ is '$TXS' with hash '$TXH'"
  echo "      B spent 1 LEZ: request $REQ, tx $TXH (public rail)  [$(elapsed)]"
  echo "EVIDENCE role=alerter step=event spender=B from=$B_PUB_B58 to=$(b58 "$DEMO_RECIPIENT_HEX") amount=1 tx=$TXH"

  echo "[6/6] alerter: A notices the change, alerts B over Logos Messaging; B reads the alert..."
  BAL1="$BAL0"; N1="$N0"; DETECTED=0
  for i in $(seq 1 30); do            # the testnet seals a block every ~60 s; 10 min budget
    R1=$(call "$A_LC" chainAccount "$B_PUB")
    BAL1=$(echo "$R1" | field balance); N1=$(echo "$R1" | field nonce)
    if [ -n "$BAL1" ] && [ "$BAL1" != "$BAL0" ]; then DETECTED=1; break; fi
    sleep 20
  done
  [ "$DETECTED" = "1" ] || fail watch "A never saw $B_PUB_B58 change (still $BAL1 after 10 min; tx $TXH)"
  BLK=""; for i in $(seq 1 10); do BLK=$(tx_block "$TXH"); [ -n "$BLK" ] && break; sleep 15; done
  [ -n "$BLK" ] || fail chain "getTransaction($TXH) is still unknown to the chain"
  ALERT="ALERT: account $B_PUB_B58 balance $BAL0 -> $BAL1 (nonce $N0 -> $N1); tx $TXH in block $BLK"
  SEND=$(call "$A_LC" messagingSend "$B_KEY" "$ALERT")
  [ "$(echo "$SEND" | field sent)" = "True" ] || [ "$(echo "$SEND" | field sent)" = "true" ] || fail alert "A's messaging.send answered: $SEND"
  INBOX=$(poll_inbox_until "$B_LC" "the alert" "m.get('kind')=='direct' and m.get('message','').startswith('ALERT: account $B_PUB_B58') for m in ms")
  echo "$INBOX" > "$OUT/agent-b-inbox.json"
  echo "      A detected $BAL0 -> $BAL1 (nonce $N0 -> $N1) and alerted B; B's inbox holds the alert  [$(elapsed)]"
  echo "EVIDENCE role=alerter step=alert watcher=A detected=\"$BAL0 -> $BAL1\" tx=$TXH block=$BLK alert_received_by=B message=\"$ALERT\""
  echo
  echo "=== ALERTER USE CASE PASSED in $(elapsed): A watched $B_PUB_B58, B's spend (tx $TXH, block $BLK) moved it $BAL0 -> $BAL1, A alerted B over Logos Messaging and B read the alert ==="
  exit 0
fi

if [ "$ROLE" = "marketplace" ]; then
  echo "[5/6] marketplace: A's private account funded by a REAL proof; B gets a language model and publishes its card; A discovers it..."
  wait_shielded "$A_LC" A "$A_PUB"
  A_PBAL="$PBAL"; A_PNONCE="$PNONCE"
  echo "EVIDENCE role=marketplace step=fund payer=A public_account=$A_PUB_B58 public_balance=$A_PBAL nonce=$A_PNONCE shielded=landed"
  call "$B_LC" metaConfigure llm.provider deepseek >/dev/null
  call "$B_LC" metaConfigure llm.api_key "$DEEPSEEK_API_KEY" >/dev/null
  call "$B_LC" metaConfigure llm.model "${PILOT_LLM_MODEL:-deepseek-v4-pro}" >/dev/null
  CARD_B=$(call "$B_LC" agentCard)
  NPK_B=$(echo "$CARD_B" | field _logos.npk)
  # The declared price lives in the card's service list (whatever its exact shape): find the
  # agent-ask entry that carries a price.
  PRICE_B=$(echo "$CARD_B" | python3 -c 'import sys,json
def walk(o):
    if isinstance(o,dict):
        if isinstance(o.get("agent-ask"),(int,float)): return o["agent-ask"]      # _logos.pricing map
        if o.get("skill") in ("agent-ask","agent.ask") or o.get("id") in ("agent-ask","agent.ask"):
            if "price" in o: return o["price"]
        for v in o.values():
            r=walk(v)
            if r is not None: return r
    elif isinstance(o,list):
        for v in o:
            r=walk(v)
            if r is not None: return r
    return None
p=walk(json.load(sys.stdin)); print("" if p is None else p)' 2>/dev/null)
  [ -n "$NPK_B" ] || fail card "B's card has no _logos.npk: $(echo "$CARD_B" | head -c 200)"
  echo "      B published its card (payment identity ${NPK_B:0:24}…, agent-ask price ${PRICE_B:-?})"
  # Discovery over the relay: A reads the discovery topic from the store (agentDiscover polls it)
  # and must come back with a card whose payment identity is B's.
  FOUND=""
  for i in $(seq 1 12); do
    DISC=$(call "$A_LC" agentDiscover "")
    if echo "$DISC" | python3 -c 'import sys,json
d=json.loads(sys.stdin.read() or "{}"); card=json.loads(sys.argv[1])
want=card.get("_logos",{}).get("npk")
sys.exit(0 if want and any(a.get("_logos",{}).get("npk")==want for a in d.get("agents",[])) else 1)' "$CARD_B" 2>/dev/null; then FOUND=1; break; fi
    sleep 10
  done
  [ -n "$FOUND" ] || fail discover "A never discovered B's card on the discovery topic (last: $(echo "$DISC" | head -c 200))"
  echo "      A discovered B's card on the discovery topic  [$(elapsed)]"
  echo "EVIDENCE role=marketplace step=discover buyer=A seller=B seller_npk=${NPK_B:0:32}… declared_price=${PRICE_B:-see-pay-step}"

  echo "[6/6] marketplace: A buys agent.ask from B, B answers, A pays B's declared price over the private rail (real proof)..."
  BAL_A0=$(private_balance "$A_LC"); BAL_B0=$(private_balance "$B_LC")
  echo "      private balances before: A=$BAL_A0 B=$BAL_B0"
  TASK=$(call "$A_LC" agentTask "$NPK_B" agent-ask '{"prompt":"In one word: what colour is the sky?"}')
  [ "$(echo "$TASK" | field state)" = "submitted" ] || fail task "agentTask answered: $TASK"
  TASK_ID=$(echo "$TASK" | field id)
  echo "      task $TASK_ID submitted; payment $(echo "$TASK" | field payment)"
  task_col() { python3 - "$A_DATA/pilot.db" "$TASK_ID" "$1" <<'PY' 2>/dev/null
import sqlite3, sys
db, task, col = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    con = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    row = con.execute("SELECT %s FROM outbound_tasks WHERE id=?;" % col, (task,)).fetchone()
    print(row[0] if row and row[0] is not None else "")
except Exception as e:
    print("")
PY
  }
  STATE=""; SP=$(( ${PAY_TIMEOUT_SECS:-7200} / 30 ))
  for j in $(seq 1 "$SP"); do
    call "$B_LC" agentPoll >/dev/null; call "$A_LC" agentPoll >/dev/null
    STATE=$(task_col state)
    case "$STATE" in paid|pay-failed|pay-unresolved|failed|canceled|rejected|accepted-nopay|awaiting-approval) break;; esac
    if [ $(( j % 10 )) -eq 0 ]; then echo "      … task $TASK_ID state '${STATE:-?}' (poll $j/$SP, $(elapsed))"; fi
    sleep 30
  done
  [ "$STATE" = "paid" ] || fail pay "task $TASK_ID ended in state '${STATE:-none}' (B's work or A's payment did not complete)"
  PRICE=$(task_col price); SPEND_ID=$(task_col spend_request_id)
  TXH=$(python3 - "$A_DATA/pilot.db" "$SPEND_ID" <<'PY' 2>/dev/null
import sqlite3, sys
con = sqlite3.connect("file:%s?mode=ro" % sys.argv[1], uri=True)
row = con.execute("SELECT tx_hash, state FROM spend_requests WHERE id=?;", (sys.argv[2],)).fetchone()
print(row[0] if row else "")
PY
  )
  [ ${#TXH} -eq 64 ] || fail pay "the payout spend $SPEND_ID has no tx hash"
  echo "      task paid: price $PRICE LEZ, spend $SPEND_ID, tx $TXH — waiting for the chain to know it..."
  BLK=""; for i in $(seq 1 40); do BLK=$(tx_block "$TXH"); [ -n "$BLK" ] && break; sleep 15; done
  [ -n "$BLK" ] || fail pay "getTransaction($TXH) is still unknown to the chain after 10 min"
  BAL_A1=""; for i in $(seq 1 12); do BAL_A1=$(private_balance "$A_LC"); [ -n "$BAL_A1" ] && [ -n "$BAL_A0" ] && [ "$BAL_A1" -lt "$BAL_A0" ] && break; sleep 10; done
  BAL_B1=$(private_balance "$B_LC")
  [ -n "$BAL_A0" ] && [ -n "$BAL_A1" ] && [ "$PRICE" -gt 0 ] && [ $(( BAL_A0 - BAL_A1 )) -eq "$PRICE" ] \
    || fail pay "A's private balance did not drop by the declared price: $BAL_A0 -> ${BAL_A1:-?} (price ${PRICE:-?})"
  echo "      ON CHAIN: tx $TXH in block $BLK; A private $BAL_A0 -> $BAL_A1 (price $PRICE); B private $BAL_B0 -> $BAL_B1  [$(elapsed)]"
  echo "EVIDENCE role=marketplace step=pay buyer=A seller=B task=$TASK_ID price=$PRICE tx=$TXH block=$BLK a_private=\"$BAL_A0 -> $BAL_A1\" b_private=\"$BAL_B0 -> $BAL_B1\" rail=private proof=real"
  echo
  echo "=== MARKETPLACE USE CASE PASSED in $(elapsed): A discovered B, bought agent.ask (task $TASK_ID), and paid B's declared $PRICE LEZ over the private rail with a real proof — tx $TXH in block $BLK; A $BAL_A0 -> $BAL_A1 ==="
  exit 0
fi

# ---- messaging ----
echo "[5/6] messaging: A -> B direct message, read back on B..."
MSG_AB="hello B, this is A ($(date -u +%T))"
SEND=$(call "$A_LC" messagingSend "$B_KEY" "$MSG_AB")
[ "$(echo "$SEND" | field sent)" = "True" ] || [ "$(echo "$SEND" | field sent)" = "true" ] || fail send "messaging.send answered: $SEND"
INBOX=$(poll_inbox_until "$B_LC" "A's direct message" "m.get('kind')=='direct' and m.get('message')=='$MSG_AB' for m in ms")
echo "      B received: $(echo "$INBOX" | python3 -c "import sys,json; ms=[m for m in json.load(sys.stdin)['messages'] if m['kind']=='direct']; print(ms[0]['message'], 'from', ms[0]['from'][:16]+'…')")  [$(elapsed)]"
echo "EVIDENCE role=messaging step=direct from=A to=B topic=$(echo "$SEND" | field topic) message=\"$MSG_AB\" received_by=B"

echo "[6/6] messaging: A creates a group and invites B (sealed invite carries the group key); B joins; one message each way..."
# One member as a plain key (the comma-list form), not a JSON array the CLI could re-parse.
GRP=$(call "$A_LC" messagingCreateGroup "$B_KEY")
GID=$(echo "$GRP" | field group_id); GTOPIC=$(echo "$GRP" | field topic)
[ -n "$GID" ] && [ "$(echo "$GRP" | field invited)" = "1" ] || fail group "messaging.create_group answered: $GRP"
INBOX=$(poll_inbox_until "$B_LC" "the group invite" "m.get('kind')=='group_invite' and m.get('group_id')=='$GID' for m in ms")
JOIN=$(call "$B_LC" messagingJoin "$GID"); case "$JOIN" in true|True|1) ;; *) fail join "B could not join $GID: '$JOIN'";; esac
echo "      B received the invite to $GID and joined ($GTOPIC)"
MSG_G_AB="group hello from A ($(date -u +%T))"
SEND=$(call "$A_LC" messagingSend "group:$GID" "$MSG_G_AB")
[ "$(echo "$SEND" | field sent)" = "True" ] || [ "$(echo "$SEND" | field sent)" = "true" ] || fail gsend "A's group send answered: $SEND"
INBOX=$(poll_inbox_until "$B_LC" "A's group message" "m.get('kind')=='group' and m.get('group_id')=='$GID' and m.get('message')=='$MSG_G_AB' for m in ms")
echo "$INBOX" > "$OUT/agent-b-inbox.json"
echo "      B received in group $GID: \"$MSG_G_AB\""
MSG_G_BA="group reply from B ($(date -u +%T))"
SEND=$(call "$B_LC" messagingSend "group:$GID" "$MSG_G_BA")
[ "$(echo "$SEND" | field sent)" = "True" ] || [ "$(echo "$SEND" | field sent)" = "true" ] || fail gsend "B's group send answered: $SEND"
INBOX=$(poll_inbox_until "$A_LC" "B's group reply" "m.get('kind')=='group' and m.get('group_id')=='$GID' and m.get('message')=='$MSG_G_BA' for m in ms")
echo "$INBOX" > "$OUT/agent-a-inbox.json"
echo "      A received in group $GID: \"$MSG_G_BA\"  [$(elapsed)]"
echo "EVIDENCE role=messaging step=group group_id=$GID topic=$GTOPIC invited=B joined=B a_to_b=\"$MSG_G_AB\" b_to_a=\"$MSG_G_BA\""
echo
echo "=== MESSAGING AGENT PASSED in $(elapsed): two funded identities ($A_PUB_B58, $B_PUB_B58); direct A->B received; group $GID created, B invited + joined, one encrypted message each way received ==="
