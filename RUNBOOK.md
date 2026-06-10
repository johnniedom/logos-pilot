# Pilot — How to Run the Demos Yourself

Open **Ubuntu (WSL)** and run these. Each demo is basically **one command**.

> Note: WSL wipes `/tmp` on reboot, but every script below rebuilds what it needs.
> The `~` scripts live in your home folder (they survive reboots).

---

## 1. Funding + File Vault + Spending Threshold  (Demos 1, 2 AND 3 — one command)

```bash
bash ~/demo-run.sh
```

Self-contained — this single command runs **three** of the four demos:
- **Demo 1 (funding):** starts the sequencer, agent funds itself → balance 100
- **Demo 2 (file vault):** encrypted upload → list → download → round-trip check
- **Demo 3 (spending threshold):** small payment auto-executes, big one is held for approval

Fast (~1–2 min, dev mode). Add ` 0` for real proofs: `bash ~/demo-run.sh 0` (slow).
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
