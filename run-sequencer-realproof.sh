#!/usr/bin/env bash
# Boot the LEZ sequencer with REAL RISC0 proofs (RISC0_DEV_MODE=0) — the mode the LP-0008
# spec requires for the on-camera demo. (The repo's run-sequencer.sh boots the Docker
# demo-sequencer in dev mode; this one runs the standalone sequencer with real proving.)
#
# PREREQUISITES (one-time — the LEZ sequencer is a separate project, not bundled here):
#   1. Clone github.com/logos-blockchain/logos-execution-zone at the rev whose circuits
#      match the installed pilot wallet module, and build the standalone sequencer:
#        cargo build --release --features standalone -p sequencer_runner
#   2. Install the RISC0 toolchain so r0vm is on PATH:   rzup install      (r0vm 3.0.5)
#   3. ~16 GB RAM recommended — real proving OOMs small boxes; lower RISC0_SEGMENT_PO2
#      (e.g. =18) to trade RAM for time.
#
# Point LEZ at your execution-zone checkout (default below).
set -euo pipefail

LEZ="${LEZ:-$HOME/dev/logos/logos-execution-zone}"
export RISC0_DEV_MODE=0                 # REAL proofs (NOT dev mode) — the on-camera requirement
export RUST_LOG="${RUST_LOG:-info}"     # so r0vm prints cycle counts (-> cu-benchmarks)

command -v r0vm >/dev/null || { echo "ERROR: r0vm not on PATH — run 'rzup install' (see Prerequisites)"; exit 1; }
SEQ="$LEZ/target/release/sequencer_runner"
[ -x "$SEQ" ] || { echo "ERROR: $SEQ not built — see Prerequisites (set LEZ=/path/to/logos-execution-zone)"; exit 1; }

echo "Booting LEZ sequencer with REAL proofs on :3040 (RISC0_DEV_MODE=0)"
cd "$LEZ"
exec "$SEQ" sequencer_runner/configs/debug   # config arg is a DIRECTORY; serves the wallet RPC on 127.0.0.1:3040
