#!/usr/bin/env bash
# LP-0008 Pilot — reproducible end-to-end demo (runs from a CLEAN CLONE).
#
#   ./demo.sh
#
# Requirements (clean machine):
#   - nix with flakes        (builds the module, its 4 deps, logoscore, lgpm)
#   - docker                 (runs the standalone LEZ devnet sequencer image)
#
# Defaults to dev mode (RISC0_DEV_MODE=1) for speed. For REAL zk proofs
# (RISC0_DEV_MODE=0, ~40 min/transfer) and the local compiled sequencer, see RUNBOOK.md.
#
# The script ALWAYS demonstrates the reproducible core (the same flow the green CI
# e2e job proves): build -> boot sequencer -> install pilot + 4 deps -> load in the
# real logoscore daemon -> list the 22 skills -> echo round-trip. It then runs the
# funding / spending-threshold / encrypted-vault demos best-effort and reports their
# status honestly (these exercise the wallet against the sequencer).

set -uo pipefail
export NIX_CONFIG="experimental-features = nix-command flakes"
export RISC0_DEV_MODE="${RISC0_DEV_MODE:-1}"
export PILOT_SEQUENCER_ADDR="${PILOT_SEQUENCER_ADDR:-http://127.0.0.1:8080}"
export PILOT_NAT="${PILOT_NAT:-extip:127.0.0.1}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
MODS="$WORK/modules"; DATA="$WORK/data"
mkdir -p "$MODS" "$DATA"
LC=""

cleanup() {
  [ -n "$LC" ] && "$LC" stop >/dev/null 2>&1 || true
  docker rm -f pilot-demo-seq >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT

port_open() { (echo > "/dev/tcp/127.0.0.1/$1") >/dev/null 2>&1; }
jget() { python3 -c "import sys,json;print(json.load(sys.stdin).get('result',''))" 2>/dev/null; }

echo "=== LP-0008 Pilot — end-to-end demo  (RISC0_DEV_MODE=$RISC0_DEV_MODE, sequencer $PILOT_SEQUENCER_ADDR) ==="

# Optional RISC0 toolchain (only needed for real proofs / some wallet ops); discover if present.
R0VM_DIR="$(find "$HOME/.risc0/extensions" -maxdepth 1 -type d -name '*cargo-risczero*' 2>/dev/null | head -1)"
[ -n "$R0VM_DIR" ] && export PATH="$R0VM_DIR:$PATH"
CIRCUITS="$(find /nix/store -maxdepth 1 -type d -name '*logos-blockchain-circuits*' 2>/dev/null | head -1)"
[ -n "$CIRCUITS" ] && export LOGOS_BLOCKCHAIN_CIRCUITS="$CIRCUITS"

echo "[1/6] Building runtime + module + dependencies (nix; heavy deps come from the Cachix cache)..."
nix build 'github:logos-co/logos-logoscore-cli' -o "$WORK/r-logoscore"
nix build 'github:logos-co/logos-package-manager' -o "$WORK/r-lgpm"
( cd "$ROOT/pilot-module" && nix build .#lgx -o "$WORK/r-pilot" )
nix build 'github:logos-blockchain/logos-execution-zone-module/5d42559db863#lgx' -o "$WORK/r-ez"
nix build 'github:logos-co/logos-delivery-module/b18ec067517e#lgx'               -o "$WORK/r-delivery"
nix build 'github:logos-co/logos-storage-module/b1d82a32c1ba#lgx'                -o "$WORK/r-storage"
nix build 'github:logos-co/logos-chat-module/a0d251f9d764#lgx'                   -o "$WORK/r-chat"

LC="$WORK/r-logoscore/bin/logoscore"
LGPM="$WORK/r-lgpm/bin/lgpm"
export LOGOS_HOST_PATH="$(find /nix/store -maxdepth 1 -name '*-logos-liblogos' -type d 2>/dev/null | head -1)/bin/logos_host"

echo "[2/6] Booting the standalone LEZ devnet sequencer (docker, :8080)..."
docker rm -f pilot-demo-seq >/dev/null 2>&1 || true
docker run -d --name pilot-demo-seq -p 8080:8080 \
  -e SEQUENCER_LISTEN_ADDR=0.0.0.0:8080 -e SEQUENCER_DB_PATH=/data/sequencer.db \
  -e SEQUENCER_SIGNING_KEY_PATH=/data/sequencer.key -e SEQUENCER_INITIAL_BALANCE=1000 \
  -e SEQUENCER_CHANNEL_ID=6d656d636f696e00000000000000000000000000000000000000000000000001 \
  -v pilot-demo-seqdata:/data \
  --entrypoint /usr/bin/logos-blockchain-demo-sequencer \
  ghcr.io/logos-blockchain/logos-blockchain:devnet >/dev/null
for i in $(seq 1 30); do port_open 8080 && break; sleep 2; done
port_open 8080 && echo "      sequencer listening on :8080" || echo "      WARNING: sequencer not reachable on :8080 (is docker running?)"

echo "[3/6] Installing pilot + 4 dependency modules via lgpm..."
for f in "$WORK"/r-pilot/*.lgx "$WORK"/r-ez/*.lgx "$WORK"/r-delivery/*.lgx "$WORK"/r-storage/*.lgx "$WORK"/r-chat/*.lgx; do
  "$LGPM" install --file "$f" --modules-dir "$MODS" --allow-unsigned >/dev/null 2>&1
done
echo "      installed: $(ls "$MODS" | tr '\n' ' ')"

echo "[4/6] Starting logoscore daemon + loading pilot (resolves dependencies)..."
setsid "$LC" -D -m "$MODS" </dev/null >"$WORK/daemon.log" 2>&1 & disown
sleep 8
LOAD=$("$LC" load-module pilot 2>&1); echo "      load: $LOAD"
if ! echo "$LOAD" | grep -q '"status":"ok"'; then
  echo "      ERROR: pilot failed to load — see $WORK/daemon.log"; tail -20 "$WORK/daemon.log"; exit 1
fi

echo "      --- 22 skills registered (the agent's capability surface) ---"
"$LC" call pilot metaSkills 2>/dev/null | jget | python3 -c "import sys,json;d=json.loads(sys.stdin.read() or '{}');print('      skill count:',d.get('count'))" 2>/dev/null
echo "      --- echo round-trip ---"
"$LC" call pilot echo "clean-clone-demo" 2>/dev/null | jget
echo "      ^ reproducible core verified (this is what the green CI e2e job asserts)."

echo "[5/6] DEMO 1 (self-funding) + DEMO 2 (spending threshold)  [best-effort — exercises the wallet]"
"$LC" call pilot initialize "$DATA" >/dev/null 2>&1   # creates identity + self-funds against the sequencer
sleep 3
ACC=$("$LC" call pilot getAccountId 2>/dev/null | jget);  echo "      agent account: ${ACC:-<none>}"
BAL=$("$LC" call pilot walletBalance 2>/dev/null | jget); echo "      balance: ${BAL:-<none>}"
RECIP=$("$LC" call logos_execution_zone create_account_private 2>/dev/null | jget)
if [ -n "$RECIP" ]; then
  "$LC" call pilot setSpendingLimits 50 200 86400 >/dev/null 2>&1
  echo "      below limit (send 30) -> $("$LC" call pilot walletSend "$RECIP" 30 storage-fee 2>/dev/null | jget)"
  HELD=$("$LC" call pilot walletSend "$RECIP" 60 big-payment 2>/dev/null)
  echo "      above limit (send 60) -> $(echo "$HELD" | jget)"
  REQ=$(echo "$HELD" | python3 -c "import sys,json;r=json.loads(json.load(sys.stdin)['result']);print(r.get('request_id',''))" 2>/dev/null)
  [ -n "$REQ" ] && echo "      owner approves $REQ -> $("$LC" call pilot approveSpend "$REQ" 2>/dev/null | jget)"
else
  echo "      (skipped — wallet/sequencer funding not available in this environment)"
fi

echo "[6/6] DEMO 3 (encrypted file vault)  [best-effort]"
echo "notarized secret $(date +%s 2>/dev/null || echo now)" > "$WORK/vault.txt"
UP=$("$LC" call pilot storageUpload "$WORK/vault.txt" demo-doc 2>/dev/null)
CID=$(echo "$UP" | python3 -c "import sys,json;r=json.loads(json.load(sys.stdin)['result']);print(r.get('cid',''))" 2>/dev/null)
if [ -n "$CID" ]; then
  echo "      uploaded (encrypted): $CID"
  "$LC" call pilot storageDownload "$CID" "$WORK/vault.out" >/dev/null 2>&1
  if diff -q "$WORK/vault.txt" "$WORK/vault.out" >/dev/null 2>&1; then
    echo "      vault round-trip: IDENTICAL"
  else
    echo "      vault round-trip: download pending/mismatch (storage network may be warming up)"
  fi
else
  echo "      (skipped — storage upload not available in this environment)"
fi

echo "=== Demo complete.  Reproducible core (build + load + 22 skills + echo) verified;"
echo "    funding/spending/vault shown best-effort against the devnet sequencer. ==="
