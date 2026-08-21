#!/usr/bin/env bash
# Boot the standalone LEZ sequencer in DEV mode on :3040 — the endpoint the pilot
# wallet actually reads (PILOT_SEQUENCER_ADDR defaults to http://127.0.0.1:3040).
#
# HISTORY: this script used to boot the old Docker demo-sequencer on :8080 — a
# different service the wallet never talks to, so funding silently failed and
# balances stayed 0 forever. If you are debugging "balance 0", first confirm your
# sequencer came from THIS script and answers on :3040.
#
# PREREQUISITES (one-time — the LEZ sequencer is a separate project, not bundled here):
#   1. Clone github.com/logos-blockchain/logos-execution-zone at the rev whose circuits
#      match the installed pilot wallet module, and build the standalone service:
#        cargo build --release --features standalone -p sequencer_service
#   2. Nothing else — dev mode (RISC0_DEV_MODE=1) fakes proofs, no prover needed.
#      For the real-proof demo use ./run-sequencer-realproof.sh instead.
#
# Fresh genesis on every start: rocksdb state is wiped so a wiped pilot wallet and a
# fresh chain always agree. A long-lived chain also makes every cold module boot
# replay thousands of blocks before the wallet answers — restart fresh instead.
#
# The catch: the wallet is NOT wiped with it. An agent funded on the previous chain
# still holds notes for accounts this new chain has never seen, and the next transfer
# aborts the wallet module mid-send ("Found new private account with non default
# values"). So this script now warns when it wipes out from under an existing wallet,
# and KEEP_STATE=1 skips the wipe when you want to keep a funded agent alive.
set -euo pipefail

LEZ="${LEZ:-$HOME/dev/logos/logos-execution-zone}"
export RISC0_DEV_MODE=1
export RUST_LOG="${RUST_LOG:-info}"

# Circuits: honor an exported override, else find a build in the nix store.
if [ -z "${LOGOS_BLOCKCHAIN_CIRCUITS:-}" ]; then
  LOGOS_BLOCKCHAIN_CIRCUITS=$(find /nix/store -maxdepth 1 -name '*logos-blockchain-circuits*' -type d 2>/dev/null | head -1)
fi
[ -n "$LOGOS_BLOCKCHAIN_CIRCUITS" ] || { echo "ERROR: set LOGOS_BLOCKCHAIN_CIRCUITS=/path/to/logos-blockchain-circuits"; exit 1; }
export LOGOS_BLOCKCHAIN_CIRCUITS

SEQ="$LEZ/target/release/sequencer_service"
[ -x "$SEQ" ] || { echo "ERROR: $SEQ not built — see Prerequisites (set LEZ=/path/to/logos-execution-zone)"; exit 1; }

# A locally-built sequencer links against a nix libstdc++ that is NOT on the default runtime
# path, so it dies instantly with "error while loading shared libraries: libstdc++.so.6".
# Prepend one from the store UNCONDITIONALLY: an `ldd` guard here reported the library as
# resolvable while the real exec still failed, so the check was worse than useless.
GCCLIB=$(ls -d /nix/store/*gcc*-lib/lib 2>/dev/null | head -1)
[ -n "$GCCLIB" ] && export LD_LIBRARY_PATH="$GCCLIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cd "$LEZ"
WALLET="${PILOT_DATA_DIR:-$HOME/.pilot}/wallet_storage.json"
if [ "${KEEP_STATE:-}" = "1" ]; then
  echo "KEEP_STATE=1 — keeping the existing chain (and any agent already funded on it)"
else
  if [ -s "$WALLET" ]; then
    echo
    echo "  !! $WALLET holds an agent funded on the chain about to be erased."
    echo "  !! Its next transfer will abort the wallet module. Pick one:"
    echo "  !!   keep the chain + agent :  KEEP_STATE=1 bash run-sequencer.sh"
    echo "  !!   start both over        :  rm -rf \"${PILOT_DATA_DIR:-$HOME/.pilot}\"/pilot.db* \\"
    echo "  !!                              \"${PILOT_DATA_DIR:-$HOME/.pilot}\"/wallet_storage.json*   then re-deploy"
    echo
  fi
  rm -rf ./rocksdb ./sequencer/service/rocksdb ./lez/sequencer/service/rocksdb \
         ./bedrock_signing_key ./sequencer/service/bedrock_signing_key \
         ./lez/sequencer/service/bedrock_signing_key 2>/dev/null || true
fi
# The 2026-06 restructure moved the service crate under lez/ — support both layouts so this
# script works at any checked-out rev.
CFG_REL="sequencer/service/configs/debug/sequencer_config.json"
[ -f "$CFG_REL" ] || CFG_REL="lez/sequencer/service/configs/debug/sequencer_config.json"
[ -f "$CFG_REL" ] || { echo "ERROR: sequencer_config.json not found in either layout"; exit 1; }
# Dev tweak carried since the old checkout: fast blocks so funding doesn't crawl — but
# BLOCK_TIME is now a knob, because 1s blocks turned out to DOS the agents (found
# 2026-08-18): the chain grows 86,400 blocks/day, every block forces the wallet to
# re-sync + re-store accounts, and the pilot module's single thread is permanently
# consumed by that churn — every RPC (agentCard, messagingSend…) starves and returns
# empty. Funding runs want BLOCK_TIME=1s; A2A/agent runs want 15s so the module can
# actually answer between blocks.
BLOCK_TIME="${BLOCK_TIME:-1s}"
sed -i -E 's/"block_create_timeout": "[0-9]+s"/"block_create_timeout": "'"$BLOCK_TIME"'"/' "$CFG_REL"
echo "Booting LEZ sequencer_service in DEV mode on :3040 (circuits: $LOGOS_BLOCKCHAIN_CIRCUITS, config: $CFG_REL)"
exec "$SEQ" "$CFG_REL"
