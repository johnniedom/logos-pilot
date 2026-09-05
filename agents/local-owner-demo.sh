#!/usr/bin/env bash
# The owner channel on ONE machine, by hand: a Pilot agent deployed here, and Basecamp's Pilot
# Remote plugin talking to it the way an owner on another machine would — through a Waku relay's
# mailbox only, never through the agent's daemon.
#
#   agents/local-owner-demo.sh            # everything below, then the lines to paste
#   agents/local-owner-demo.sh --stop     # stop the relay and the agent daemon it started
#
# What it does:
#   1. relay    an nwaku relay on 127.0.0.1 (REST :8645, tcp :30303). Reuses one already
#               running; else Docker if `docker` works; else `nix run github:waku-org/nwaku`
#               (builds nwaku from source the first time — slow).
#   2. owner    builds the console client and the pilot_owner module; installs the module and
#               the two plugins into Basecamp (install-basecamp.sh); makes the owner key with
#               `pilot-owner init`. The module and the console client share ~/.pilot-owner, so
#               Basecamp will find the key already made.
#   3. agent    `pilot deploy --testnet`, dialled at the relay, bound to the owner key
#               (PILOT_OWNER_NPK), headless (no LLM unless PILOT_LLM_PROVIDER is set). Waits for
#               the identity, prints the agent's card and account, and pairs the console client.
#   4. you      open Basecamp -> Pilot Remote. It shows "Paired" (same state file). Type
#               /balance, then /send public:<recipient> 101 test -> the agent holds it and asks;
#               /approve <id> -> the spend lands on the public testnet. Or press "Change agent"
#               and paste the printed card + account + relay yourself to see the set-up screen.
#
# One agent at a time on this box: the CLI daemon's modules and Basecamp's own agent modules
# clash on ports, so do NOT open "Pilot Agent" (the local plugin) while this runs. Pilot Remote
# declares only pilot_owner, so Basecamp loads no agent module for it.
#
# Needs: nix, python3, curl; the pilot module stack already in the nix store (deploy installs it
# from there — see ~/build-testnet.lf.sh on the dev box) or PILOT_MODULE_PATH pointing at
# installed modules. Env honoured: PILOT_DATA_DIR (default ~/.pilot), PILOT_MODULE_PATH
# (default ~/.pilot/modules), LEZ_RPC, PILOT_LLM_PROVIDER, RISC0_DEV_MODE (default 1).

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PILOT_DATA_DIR="${PILOT_DATA_DIR:-$HOME/.pilot}"
export PILOT_MODULE_PATH="${PILOT_MODULE_PATH:-$HOME/.pilot/modules}"
export RISC0_DEV_MODE="${RISC0_DEV_MODE:-1}"
export PILOT_LLM_PROVIDER="${PILOT_LLM_PROVIDER:-none}"
LEZ_RPC="${LEZ_RPC:-https://testnet.lez.logos.co}"
export PILOT_SEQUENCER_ADDR="$LEZ_RPC"
REST="http://127.0.0.1:8645"
STATE="$HOME/.pilot-owner/state.json"
RUN="$PILOT_DATA_DIR/local-owner-demo"; mkdir -p "$RUN"

say()  { echo; echo "== $*"; }
die()  { echo; echo "STOP: $*"; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"; }

if [ "${1:-}" = "--stop" ]; then
  [ -f "$RUN/relay.pid" ] && kill "$(cat "$RUN/relay.pid")" 2>/dev/null && echo "relay stopped"
  docker rm -f pilot-nwaku >/dev/null 2>&1 && echo "relay container removed"
  [ -x "$RUN/pilot" ] && "$RUN/pilot" stop >/dev/null 2>&1 && echo "agent daemon stopped"
  exit 0
fi

need nix; need python3; need curl
export NIX_CONFIG="experimental-features = nix-command flakes"

say "[1/4] Relay"
if curl -s -m 3 "$REST/debug/v1/info" >/dev/null 2>&1; then
  echo "   a relay already answers on $REST — using it"
elif docker info >/dev/null 2>&1; then
  docker rm -f pilot-nwaku >/dev/null 2>&1 || true
  docker run -d --name pilot-nwaku -p 127.0.0.1:8645:8645 -p 30303:30303/tcp -p 30303:30303/udp \
    harbor.status.im/wakuorg/nwaku:v0.38.0 \
    --tcp-port=30303 --rest=true --rest-address=0.0.0.0 --rest-port=8645 --rest-admin=true \
    --relay=true --cluster-id=2 --num-shards-in-network=8 \
    --store=true --store-message-db-url=sqlite:///store.sqlite3 \
    --discv5-discovery=false --nat=extip:127.0.0.1 >/dev/null || die "docker could not start nwaku"
  echo "   nwaku container started"
else
  echo "   no relay, no docker: starting nwaku with nix (first time builds it from source)"
  nohup nix run 'github:waku-org/nwaku' -- \
    --tcp-port=30303 --rest=true --rest-address=127.0.0.1 --rest-port=8645 --rest-admin=true \
    --relay=true --cluster-id=2 --num-shards-in-network=8 \
    --store=true --store-message-db-url="sqlite://$RUN/store.sqlite3" \
    --discv5-discovery=false --nat=extip:127.0.0.1 > "$RUN/relay.log" 2>&1 &
  echo $! > "$RUN/relay.pid"
fi
for i in $(seq 1 90); do curl -s -m 3 "$REST/debug/v1/info" >/dev/null 2>&1 && break; sleep 5; done
ID=$(curl -s -m 5 "$REST/debug/v1/info" | python3 -c "import sys,json;print(json.load(sys.stdin)['listenAddresses'][0].rsplit('/p2p/',1)[1])" 2>/dev/null)
[ -n "$ID" ] || die "the relay never answered on $REST (see $RUN/relay.log)"
export PILOT_WAKU_ADDR="/ip4/127.0.0.1/tcp/30303/p2p/$ID"
export PILOT_WAKU_REST="$REST"
echo "   relay: $PILOT_WAKU_ADDR"

say "[2/4] Owner side: console client, pilot_owner module, Basecamp install, owner key"
nix build "$ROOT/pilot-owner" -o "$RUN/r-owner" || die "nix build ./pilot-owner failed"
OWNER="$RUN/r-owner/bin/pilot-owner"
"$OWNER" selftest || die "pilot-owner selftest failed"
nix build "$ROOT/pilot-owner/module#lgx" -o "$ROOT/pilot-owner/module/result-lgx" || die "nix build ./pilot-owner/module#lgx failed"
bash "$ROOT/install-basecamp.sh" | sed 's/^/   /'
INIT=$("$OWNER" init)
echo "$INIT" | sed 's/^/   /'
OWNER_PUB=$(python3 -c 'import sys,json; print(json.load(open(sys.argv[1]))["owner_pub"])' "$STATE")
[ ${#OWNER_PUB} -ge 66 ] || die "no owner key in $STATE"

say "[3/4] Agent: pilot deploy --testnet, bound to the owner key, dialled at the relay"
if [ -n "${PILOT_BIN:-}" ]; then cp "$PILOT_BIN" "$RUN/pilot"; else
  nix build "$ROOT/pilot-cli" -o "$RUN/r-cli" || die "nix build ./pilot-cli failed"
  ln -sfn "$RUN/r-cli/bin/pilot" "$RUN/pilot"
fi
export PILOT_OWNER_NPK="$OWNER_PUB"
export LOGOS_HOST_PATH="${LOGOS_HOST_PATH:-$(find /nix/store -maxdepth 1 -name '*-logos-liblogos' -type d 2>/dev/null | head -1)/bin/logos_host}"
CIRCUITS="$(find /nix/store -maxdepth 1 -type d -name '*logos-blockchain-circuits*' 2>/dev/null | head -1)"
[ -n "$CIRCUITS" ] && export LOGOS_BLOCKCHAIN_CIRCUITS="$CIRCUITS"
echo "   data $PILOT_DATA_DIR, modules $PILOT_MODULE_PATH"
"$RUN/pilot" deploy --testnet 2>&1 | tee "$RUN/deploy.log" | sed 's/^/   /'
LC_BIN="${LOGOSCORE:-$(command -v logoscore || true)}"
LCDIR="$PILOT_DATA_DIR/.logoscore"
if [ -n "$LC_BIN" ]; then
  ACCOUNT=""; for i in $(seq 1 60); do
    ACCOUNT=$("$LC_BIN" --config-dir "$LCDIR" call pilot getAccountId 2>/dev/null | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("result",""))
except Exception: print("")')
    [ ${#ACCOUNT} -eq 64 ] && break; sleep 10
  done
  "$LC_BIN" --config-dir "$LCDIR" call pilot agentCard 2>/dev/null | python3 -c 'import sys,json
r=json.load(sys.stdin).get("result",""); print(r if isinstance(r,str) else json.dumps(r))' > "$RUN/agent-card.json"
  "$LC_BIN" --config-dir "$LCDIR" call pilot establishOwnerChannel >/dev/null 2>&1
else
  echo "   (logoscore not on PATH: read the card with 'pilot card' and the account with 'pilot status')"
  "$RUN/pilot" card > "$RUN/agent-card.json" 2>/dev/null || true
  ACCOUNT=$("$RUN/pilot" status 2>/dev/null | grep -oE '[0-9a-f]{64}' | head -1)
fi
[ ${#ACCOUNT} -eq 64 ] || die "no agent account yet (deploy log: $RUN/deploy.log)"
[ -s "$RUN/agent-card.json" ] || die "no agent card yet"
"$OWNER" pair "$RUN/agent-card.json" "$ACCOUNT" --relay "$REST" | sed 's/^/   /'

say "[4/4] Your turn"
cat <<TXT
   Open Basecamp -> "Pilot Remote". It reads ~/.pilot-owner/state.json, so it already shows Paired.
   Press /balance. Then:
      /send public:f8fc394c0e5440c4188236d1693076b0cfad04984cf67ca64e0e43a173144f63 101 basecamp test
   -> the agent holds it (above the 100-LEZ limit) and answers with "/approve <id>". Send that line.
   -> the spend lands on the public testnet; check with:  $OWNER listen --since 300

   To see the set-up screen instead: "Change agent" and paste
      card:     $RUN/agent-card.json  (its contents)
      account:  $ACCOUNT
      relay:    $REST

   Console alternative:  $OWNER send "/balance"; $OWNER listen --since 120
   Stop everything:      agents/local-owner-demo.sh --stop
TXT
