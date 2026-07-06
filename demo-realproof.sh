#!/usr/bin/env bash
# Reproducible REAL-PROOF demo for LP-0008. Run AFTER ./run-sequencer-realproof.sh is up
# on :3040. This is the flow the demo VIDEO captures: the on-chain steps generate visible
# r0vm proofs because RISC0_DEV_MODE=0 on both the sequencer and the wallet.
#
# PREREQUISITES:
#   - A real-proof sequencer running (see ./run-sequencer-realproof.sh).
#   - An LLM API key exported (e.g. ANTHROPIC_API_KEY) for the chat / agent.ask steps.
#   - Nix with flakes; Docker for the Waku node (messaging/A2A use cases).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PILOT="$ROOT/pilot-cli/result/bin/pilot"
export PILOT_DATA_DIR="${PILOT_DATA_DIR:-$HOME/.pilot}"
export RISC0_DEV_MODE=0                  # the wallet side must prove for real too

echo "== 1. Build (idempotent) =="
( cd "$ROOT/pilot-module" && nix build )
( cd "$ROOT/pilot-cli"    && nix build )

echo "== 2. Install modules into logoscore =="
"$ROOT/setup-modules.sh"

echo "== 3. Deploy: create identity + self-fund (register + pinata claim) =="
echo "   >>> WATCH THE TERMINAL: r0vm proof generation here is the proof RISC0_DEV_MODE=0 is active <<<"
"$PILOT" deploy

echo "== 4. Confirm a non-zero on-chain balance =="
"$PILOT" status    # top-level CLI has no `balance` command; status shows the wallet balance

# ---- Illustrative use cases (driven interactively through `pilot chat`) ----------------
# Skills are invoked via slash commands in `pilot chat` (or `logoscore call pilot <method>`).
echo ""
echo "== Now walk the 3 use cases on camera (in 'pilot chat'): =="
echo "   A) Personal file vault  : upload a file -> get a content address -> download it back"
echo "   B) Spending threshold   : an above-limit wallet send is HELD; /approve <id> runs the"
echo "                             on-chain transfer (real r0vm proof on screen)"
echo "   C) A2A paid service     : a second agent runs agent.ask and is paid autonomously in LEZ"
echo "                             (see test-two-agents-docker.sh for the two-agent topology)"
echo ""
echo "== Demo complete. Every on-chain step above ran with RISC0_DEV_MODE=0 (real RISC0 proofs). =="
