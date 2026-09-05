#!/usr/bin/env bash
# LP-0008 — "deploy and configure with a single headless CLI command", shown on a machine that has
# never seen the agent: `pilot deploy --testnet` with the provider, model and owner key in the env,
# no keystrokes, no setup-modules.sh. The deploy must install the four modules itself, create the
# identity, and leave a daemon that funds itself from the faucet; then the owner talks to it from
# the separate pilot-owner client over the relay. Every step asserted.
#
#   agents/headless-deploy.sh
#
# Env: see agents/lib.sh (LOGOSCORE_BIN/LGPM_BIN, LEZ_RPC); PILOT_BIN / OWNER_BIN = pre-built
# binaries (else built here with nix).

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib.sh
. "$ROOT/agents/lib.sh"
need nix; need python3; need curl

OUT="$ROOT/agents/out/headless"; mkdir -p "$OUT"
rm -f "$OUT"/deploy.log "$OUT"/status.log "$OUT"/daemon.log "$OUT"/agent-state.txt "$OUT"/agent-card.json "$OUT"/owner-transcript.txt
WORK="$(mktemp -d)"
export PILOT_DATA_DIR="$WORK/data"                 # where deploy puts the agent (pilot.db, wallet, daemon)
export PILOT_MODULE_PATH="$WORK/modules"           # EMPTY: deploy has to fill it
export PILOT_OWNER_HOME="$WORK/owner-home"
LCDIR="$PILOT_DATA_DIR/.logoscore"                 # the CLI's default config dir under the data dir
A_LOG="$PILOT_DATA_DIR/daemon.log"
FAIL_LOGS="$A_LOG"
# The daemon log lands in $OUT from its first byte. The CLI writes it under the data dir and the
# cleanup below copied it at exit, but a job cancelled at its cap never reaches the cleanup: runs
# 33944283880 and 33963022638 kept no daemon log at all. The symlink puts it in the artifact
# whatever happens to the job.
mkdir -p "$PILOT_DATA_DIR"; ln -sfn "$OUT/daemon.log" "$A_LOG"
# A module that answers no RPC still leaves time for the cleanup (log + state dump) before the
# workflow cap of 150 min: an unanswered poll costs ~66 s (20 s sleep + the daemon abandoning the
# call), so 75 polls is ~83 min on top of the ~45 min of build + deploy.
FUND_TIMEOUT_SECS="${FUND_TIMEOUT_SECS:-1500}"

# What the module itself recorded, straight from its database: funding.last_error,
# funding.public_account, funded — readable even when the module answers nothing. Secret-bearing
# keys are redacted by name.
dump_agent_state() {
  local DB="$PILOT_DATA_DIR/pilot.db"
  {
    echo "pilot.db: $(ls -la "$DB" 2>&1)"
    echo "wallet_storage.json: $(stat -c %s "$PILOT_DATA_DIR/wallet_storage.json" 2>/dev/null || echo absent) bytes"
    [ -f "$DB" ] && python3 - "$DB" <<'PY'
import sqlite3, sys
try:
    con = sqlite3.connect("file:%s?mode=ro" % sys.argv[1], uri=True)
    for k, v in con.execute("SELECT key, value FROM config ORDER BY key;"):
        if any(s in k for s in ("priv", "api_key", "secret", "passphrase")): v = "<redacted>"
        print("config %s = %s" % (k, str(v)[:200]))
    for row in con.execute("SELECT id, state, amount, tx_hash, error, updated_at FROM spend_requests ORDER BY updated_at DESC LIMIT 5;"):
        print("spend %s" % (row,))
except Exception as e:
    print("dump failed: %s" % e)
PY
  } > "$OUT/agent-state.txt" 2>&1
  echo "      agent state dumped to agents/out/headless/agent-state.txt ($(grep -c "^config" "$OUT/agent-state.txt" 2>/dev/null) config rows)"
}

cleanup() {
  stop_daemon "$LCDIR"
  [ -L "$A_LOG" ] || cp "$A_LOG" "$OUT/daemon.log" 2>/dev/null || true
  dump_agent_state
  [ "${KEEP_RELAY:-0}" = "1" ] || stop_relay
  rm -rf "$WORK"
}
trap cleanup EXIT

echo "=== LP-0008 Pilot — headless deploy on a fresh machine  (endpoint $LEZ_RPC) ==="
chain_check

echo "[1/5] Building the module stack (the LGX packages land in the nix store), the CLI and the owner client..."
build_all "$WORK" "$ROOT"
export LOGOSCORE="$LC"                             # the CLI honours LOGOSCORE / LOGOS_HOST_PATH
if [ -n "${PILOT_BIN:-}" ]; then PILOT="$PILOT_BIN"; else
  nix build "$ROOT/pilot-cli" -o "$WORK/r-cli" || fail build "pilot-cli"
  PILOT="$WORK/r-cli/bin/pilot"
fi
if [ -n "${OWNER_BIN:-}" ]; then OWNER="$OWNER_BIN"; else
  nix build "$ROOT/pilot-owner" -o "$WORK/r-owner" || fail build "pilot-owner"
  OWNER="$WORK/r-owner/bin/pilot-owner"
fi
"$PILOT" version | sed 's/^/      /'
echo "      modules dir before deploy: $(ls -A "$PILOT_MODULE_PATH" 2>/dev/null | wc -l) entries (must be 0)"
[ ! -e "$PILOT_MODULE_PATH/pilot" ] || fail fresh "modules dir is not empty before deploy"

echo "[2/5] Starting the local relay; making the owner's key (the only two things that are not the one command)..."
start_relay
INIT=$("$OWNER" init); OWNER_PUB=$(echo "$INIT" | sed -n 's/^owner public key: //p')
[ ${#OWNER_PUB} -ge 66 ] || fail owner "pilot-owner init printed no key"

echo "[3/5] ONE command, no keystrokes: pilot deploy --testnet (provider none, owner key from the env)..."
export PILOT_LLM_PROVIDER=none PILOT_OWNER_NPK="$OWNER_PUB" RISC0_DEV_MODE=1 PILOT_NAT=extip:127.0.0.1
T0=$(date +%s)
"$PILOT" deploy --testnet </dev/null > "$OUT/deploy.log" 2>&1
DEPLOY_RC=$?
sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$OUT/deploy.log" | grep -v "^\s*$" | tail -30 | sed 's/^/      deploy: /'
echo "      deploy exit $DEPLOY_RC after $(( ($(date +%s) - T0) / 60 ))m"
CLEAN=$(sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$OUT/deploy.log")
for m in pilot lez_core delivery_module storage_module; do
  echo "$CLEAN" | grep -q "installed $m from" || fail install "deploy did not report installing $m"
  [ -d "$PILOT_MODULE_PATH/$m" ] || fail install "$m is not in $PILOT_MODULE_PATH after deploy"
done
echo "$CLEAN" | grep -q "Agent identity created" || fail deploy "deploy did not create the identity (see deploy.log)"
echo "      deploy installed the four modules itself and created the identity"
echo "EVIDENCE role=headless step=deploy command=\"pilot deploy --testnet\" installed=\"pilot lez_core delivery_module storage_module\" identity=created exit=$DEPLOY_RC"

echo "[4/5] The daemon deploy left behind funds the agent from the faucet (chain read)..."
wait_funded "$LCDIR" "$PILOT_DATA_DIR" agent
wait_quiet "$LCDIR" agent
"$PILOT" status </dev/null 2>&1 | sed 's/\x1b\[[0-9;]*[A-Za-z]//g' | tee "$OUT/status.log" | grep -v "^\s*$" | sed 's/^/      status: /'
echo "EVIDENCE role=headless step=funded account=$ACCOUNT public_account=$PUB_B58 balance=$PBAL nonce=$PNONCE"

echo "[5/5] The owner talks to the deployed agent from the separate client over the relay..."
call "$LCDIR" agentCard > "$OUT/agent-card.json"
R=$(call "$LCDIR" establishOwnerChannel); case "$R" in true|True|1) ;; *) fail bind "establishOwnerChannel answered '$R'";; esac
"$OWNER" pair "$OUT/agent-card.json" "$ACCOUNT" --relay "$PILOT_WAKU_REST" | sed 's/^/      owner: /'
SENT=$("$OWNER" send "/balance") || fail send "pilot-owner send failed: $SENT"
REPLY=""
for i in $(seq 1 30); do
  call "$LCDIR" agentPoll >/dev/null
  REPLY=$("$OWNER" listen --since 900)
  echo "$REPLY" | grep -q '"balance"' && break
  sleep 5
done
echo "$REPLY" > "$OUT/owner-transcript.txt"
echo "$REPLY" | grep -q '"balance"' || fail receive "the owner client never received the agent's /balance reply (transcript: $(echo "$REPLY" | tail -2 | head -c 200))"
echo "$REPLY" | grep '"balance"' | head -1 | sed 's/^/      agent -> owner: /'
echo "EVIDENCE role=headless step=owner_reply command=/balance received_by=pilot-owner relay=$PILOT_WAKU_REST"
echo
echo "=== HEADLESS DEPLOY PASSED in $(elapsed): one command installed the modules, created the identity, funded itself ($PUB_B58 = $PBAL LEZ on chain); the owner client got its balances over the relay ==="
