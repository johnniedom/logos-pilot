# Pilot — How to Run the Demos Yourself

Open **Ubuntu (WSL)** and run these. Each demo is basically **one command**.

> Note: WSL wipes `/tmp` on reboot, but every script below rebuilds what it needs.
> The `~` scripts live in your home folder (they survive reboots).

---

## 1. Funding + File Vault + Spending Threshold  (Demos 1, 2 AND 3 — one command)

**Reproducible, from a clean clone** (requires `nix` + `docker`):

```bash
git clone <repo> && cd logos-pilot
./demo.sh
```

`./demo.sh` builds the module + its 4 dependencies + logoscore (heavy deps come
from the Cachix cache), boots the standalone **LEZ devnet sequencer via Docker
(:8080)**, loads the agent in the real logoscore daemon, and runs:
- **Reproducible core** (the same flow CI's e2e job asserts): load pilot + deps →
  23 skills → echo round-trip.
- **Demo 1 (funding):** agent self-funds against the sequencer → balance.
- **Demo 2 (spending threshold):** small payment auto-executes, big one is held for
  owner approval, then approved.
- **Demo 3 (file vault):** encrypted upload → download → byte-identical round-trip.

Dev mode (`RISC0_DEV_MODE=1`) by default. The funding/spending/vault steps are
best-effort and report their status honestly.

> **Real zk proofs (`RISC0_DEV_MODE=0`) + the local compiled sequencer (:3040):**
> see §3 below — these take ~40 min/transfer and are for the recorded video, not
> the quick reproducible run.

For the per-step commands to type on screen, see `pilot-module/demo/DEMO.md`.

---

## 2. Two-Agent Payment (A2A)  — needs Docker

First make sure **Docker Desktop is open**, then start the shared Waku node and run it:

```bash
cd ~/dev/logos/logos-pilot
docker compose up -d nwaku        # one-time per session; wait ~20s for it to be ready
bash ~/run-a2a.sh                 # Agent A (host) + Agent B (Docker) discover, message, pay
```

Expect: discover → bidirectional messages → storage share → A2A task → A→B payment.

---

## 3. Real Proofs  (for the video — RISC0_DEV_MODE=0)

Two terminals:

```bash
# Terminal 1 — start the real-proof sequencer (leave it running)
bash ~/run-sequencer-realproof.sh

# Terminal 2 — once Terminal 1 is printing "Block ... created", run the proof demo
bash ~/demo-realproof.sh
```

**Rehearse first.** `REHEARSE=1` walks the identical path with fake proofs, so a dry run
takes minutes instead of ~40 per transfer. Both terminals print a loud banner in that mode
and the closing line says "FAKE proofs", so you can't record one by accident:

```bash
# Terminal 1
REHEARSE=1 KEEP_STATE=1 bash ~/run-sequencer-realproof.sh
# Terminal 2
REHEARSE=1 KEEP_STATE=1 bash ~/demo-realproof.sh
```

**Give both terminals the same `REHEARSE` and `KEEP_STATE` settings.** The agent's wallet
only means anything on the chain it was funded against, so the two must match:

| | Terminal 1 (sequencer) | Terminal 2 (demo) |
|---|---|---|
| **Recording a take** (no flags) | real proofs, wipes to a fresh chain | real proofs, fresh wallet in `~/.pilot-demo`, self-funds on camera |
| **Rehearsal** (`REHEARSE=1 KEEP_STATE=1`) | fake proofs, keeps the chain | fake proofs, keeps the wallet |

Mixing them — a fresh chain with a wallet from the previous one — crashes the agent with
`Found new private account with non default values`. The demo keeps its wallet in
`~/.pilot-demo` and never touches your everyday agent in `~/.pilot`; the old wallet is
moved to `~/.pilot-demo.bak-<timestamp>` rather than deleted.

This generates a **real zk proof**: funds a public account → 150, then a shielded
public→private transfer of 77 → **private balance = 77**.
⚠️ **Slow** — the shielded proof takes ~40 min on a 7.6 GB-RAM laptop (RAM-bound; minutes on 16 GB+).
The `r0vm` activity in Terminal 1/2 is the proof being built — that's the thing to record.

When done, stop the sequencer:
```bash
pkill sequencer_servi
```

---

## Quick health checks

```bash
df -h /mnt/c | tail -1          # Windows C: free space (keep an eye on this)
pgrep -a sequencer_servi        # is a sequencer running?
```
