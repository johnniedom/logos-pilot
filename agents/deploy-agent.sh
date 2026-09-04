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
case "$ROLE" in storage|messaging|blockchain) ;; *) echo "usage: $0 --role storage|messaging|blockchain"; exit 2;; esac

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

OUT="$ROOT/agents/out/$ROLE"; rm -rf "$OUT"; mkdir -p "$OUT"
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
# Kick both initializes, then wait for each: the two wallets sync in parallel.
"$LC" --config-dir "$A_LC" call pilot initialize "$A_DATA" >/dev/null 2>&1 || true
"$LC" --config-dir "$B_LC" call pilot initialize "$B_DATA" >/dev/null 2>&1 || true
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
  CONN=$(call "$B_LC" storageConnect "$A_PEER" "[\"$A_STORAGE_ADDR\"]")
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

# ---- messaging ----
echo "[5/6] messaging: A -> B direct message, read back on B..."
MSG_AB="hello B, this is A ($(date -u +%T))"
SEND=$(call "$A_LC" messagingSend "$B_KEY" "$MSG_AB")
[ "$(echo "$SEND" | field sent)" = "True" ] || [ "$(echo "$SEND" | field sent)" = "true" ] || fail send "messaging.send answered: $SEND"
INBOX=$(poll_inbox_until "$B_LC" "A's direct message" "m.get('kind')=='direct' and m.get('message')=='$MSG_AB' for m in ms")
echo "      B received: $(echo "$INBOX" | python3 -c "import sys,json; ms=[m for m in json.load(sys.stdin)['messages'] if m['kind']=='direct']; print(ms[0]['message'], 'from', ms[0]['from'][:16]+'…')")  [$(elapsed)]"
echo "EVIDENCE role=messaging step=direct from=A to=B topic=$(echo "$SEND" | field topic) message=\"$MSG_AB\" received_by=B"

echo "[6/6] messaging: A creates a group and invites B (sealed invite carries the group key); B joins; one message each way..."
GRP=$(call "$A_LC" messagingCreateGroup "[\"$B_KEY\"]")
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
