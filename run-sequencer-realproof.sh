#!/usr/bin/env bash
# Boot the LEZ sequencer with REAL RISC0 proofs (RISC0_DEV_MODE=0) — the mode the LP-0008
# spec requires for the on-camera demo. (The repo's run-sequencer.sh boots the Docker
# demo-sequencer in dev mode; this one runs the standalone sequencer with real proving.)
#
# PREREQUISITES (one-time — the LEZ sequencer is a separate project, not bundled here):
#   1. Clone github.com/logos-blockchain/logos-execution-zone at the rev whose circuits
#      match the installed pilot wallet module, and build the standalone service:
#        cargo build --release --features standalone -p sequencer_service
#   2. Install the RISC0 toolchain so r0vm is on PATH:   rzup install      (r0vm 3.0.5)
#   3. ~16 GB RAM recommended — real proving OOMs small boxes; lower RISC0_SEGMENT_PO2
#      (e.g. =18) to trade RAM for time.
#
# Point LEZ at your execution-zone checkout (default below).
set -euo pipefail

LEZ="${LEZ:-$HOME/dev/logos/logos-execution-zone}"
export RUST_LOG="${RUST_LOG:-info}"     # so r0vm prints cycle counts (-> cu-benchmarks)

# REHEARSE=1 walks the exact same path with fake proofs, so a dry run costs minutes instead
# of ~40 per transfer. It is NOT valid for the recording — the spec requires real proving.
# Pass the same REHEARSE / KEEP_STATE values to demo-realproof.sh.
if [ "${REHEARSE:-}" = "1" ]; then
  export RISC0_DEV_MODE=1
  echo "################################################################"
  echo "#  REHEARSAL — RISC0_DEV_MODE=1, proofs are FAKE. Do NOT record. #"
  echo "################################################################"
else
  export RISC0_DEV_MODE=0               # REAL proofs (NOT dev mode) — the on-camera requirement
fi

# RISC0 prover toolchain: honor an already-exported PATH, else discover an installed extension.
R0VM_DIR="$(find "$HOME/.risc0/extensions" -maxdepth 1 -type d -name '*cargo-risczero*' 2>/dev/null | head -1)"
[ -n "$R0VM_DIR" ] && export PATH="$R0VM_DIR:$PATH"
command -v r0vm >/dev/null || { echo "ERROR: r0vm not on PATH — run 'rzup install' (see Prerequisites)"; exit 1; }

# Circuits: honor an exported override, else find a build in the nix store.
if [ -z "${LOGOS_BLOCKCHAIN_CIRCUITS:-}" ]; then
  LOGOS_BLOCKCHAIN_CIRCUITS=$(find /nix/store -maxdepth 1 -name '*logos-blockchain-circuits*' -type d 2>/dev/null | head -1)
fi
[ -n "$LOGOS_BLOCKCHAIN_CIRCUITS" ] || { echo "ERROR: set LOGOS_BLOCKCHAIN_CIRCUITS=/path/to/logos-blockchain-circuits"; exit 1; }
export LOGOS_BLOCKCHAIN_CIRCUITS

SEQ="$LEZ/target/release/sequencer_service"
[ -x "$SEQ" ] || { echo "ERROR: $SEQ not built — see Prerequisites (set LEZ=/path/to/logos-execution-zone)"; exit 1; }
CONFIG="$LEZ/sequencer/service/configs/debug/sequencer_config.json"
[ -f "$CONFIG" ] || { echo "ERROR: $CONFIG not found — see Prerequisites"; exit 1; }

echo "Booting LEZ sequencer on :3040 (RISC0_DEV_MODE=$RISC0_DEV_MODE)"
cd "$LEZ"
# KEEP_STATE=1 preserves the existing chain + already-funded wallet across restarts (for
# rehearsals). Unset (the default) wipes to a fresh genesis — what the recorded demo wants.
if [ "${KEEP_STATE:-}" = "1" ]; then
  echo "KEEP_STATE=1 — preserving existing chain + funded wallet (no wipe)"
else
  echo "Wiping to fresh genesis (set KEEP_STATE=1 to keep the funded wallet across restarts)"
  rm -rf ./rocksdb ./sequencer/service/rocksdb ./bedrock_signing_key ./sequencer/service/bedrock_signing_key 2>/dev/null || true
fi
exec "$SEQ" sequencer/service/configs/debug/sequencer_config.json   # serves the wallet RPC on 127.0.0.1:3040
