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

# REHEARSE=1 = dress rehearsal with fake proofs (minutes, not ~40 per transfer). Give the
# sequencer script the same REHEARSE / KEEP_STATE values — both sides must agree.
if [ "${REHEARSE:-}" = "1" ]; then
  export RISC0_DEV_MODE=1
  echo "################################################################"
  echo "#  REHEARSAL — RISC0_DEV_MODE=1, proofs are FAKE. Do NOT record. #"
  echo "################################################################"
else
  export RISC0_DEV_MODE=0                # the wallet side must prove for real too
fi

# The agent's wallet only means anything on the chain it was funded against. Boot a fresh
# genesis (run-sequencer-realproof.sh with KEEP_STATE unset) while pointing at a wallet that
# still holds notes from the previous chain and the transfer circuit aborts the module:
#   "Found new private account with non default values" -> logos_execution_zone crash.
# So the SAME KEEP_STATE switch drives both scripts, and this demo keeps its own data dir
# instead of the daily agent's ~/.pilot:
#   (unset)       fresh genesis   + fresh wallet, self-funded on camera  <- the recorded demo
#   KEEP_STATE=1  existing chain  + existing wallet                      <- quick rehearsals
# Modules stay in ~/.pilot/modules (setup-modules.sh's default) — only wallet state is reset.
export PILOT_DATA_DIR="${PILOT_DATA_DIR:-$HOME/.pilot-demo}"
export PILOT_MODULE_PATH="${PILOT_MODULE_PATH:-$HOME/.pilot/modules}"

echo "== 0. Preflight =="
curl -s -m 5 -o /dev/null -X POST "http://127.0.0.1:3040" -H 'content-type: application/json' -d '{}' \
  || { echo "ERROR: no sequencer answering on :3040 — start ./run-sequencer-realproof.sh first"; exit 1; }
echo "   sequencer: up on :3040"

if [ "${KEEP_STATE:-}" = "1" ]; then
  echo "   KEEP_STATE=1 — reusing the wallet in $PILOT_DATA_DIR (boot the sequencer with KEEP_STATE=1 too)"
else
  if [ -e "$PILOT_DATA_DIR" ]; then
    ARCHIVE="$PILOT_DATA_DIR.bak-$(date +%Y%m%d-%H%M%S)"
    mv "$PILOT_DATA_DIR" "$ARCHIVE"
    echo "   previous wallet archived -> $ARCHIVE"
  fi
  mkdir -p "$PILOT_DATA_DIR"
  echo "   fresh wallet in $PILOT_DATA_DIR (pair with a fresh-genesis sequencer)"
fi
echo "   modules: $PILOT_MODULE_PATH"

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
if [ "$RISC0_DEV_MODE" = "0" ]; then
  echo "== Demo complete. Every on-chain step above ran with RISC0_DEV_MODE=0 (real RISC0 proofs). =="
else
  echo "== Rehearsal complete (RISC0_DEV_MODE=1 — FAKE proofs). Re-run without REHEARSE=1 to record. =="
fi
