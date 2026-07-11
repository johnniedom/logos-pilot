# Pilot — Troubleshooting Field Guide

Symptom-indexed fixes for everything that has actually broken in the field.
Most "broken" states here are really **two components remembering different
histories** — a wallet and a chain, a daemon and its environment, a script and
a leftover container. Reset them *together* and they stop disagreeing.

Last verified: 2026-07-11.

---

## Chat prints nothing at all (commands and messages both)

**Symptom:** `pilot chat` shows "Agent online", but `/balance` or plain chat
returns silence.

**Cause:** the wallet is replaying the chain. On a cold boot the wallet must
re-read every block minted since it last ran, and the module answers nothing
until it catches up. A sequencer left running for a day mints thousands of
blocks (~0.7/s), so the catch-up can take minutes. Every call you make waits
in line behind it, times out, and renders as silence.

**Check:** `tail ~/.pilot/daemon.log` — look for `Syncing to block N. Blocks
to sync: M` and `Synced to block N in …s`.

**Fix:** wait for the sync to finish, then retry. Between demo takes, stay in
the same chat session — every restart re-pays the replay tax. Don't leave the
dev sequencer running for days.

---

## `/approve` says it failed — did money move?

`Approve Failed — not approved: request unknown, already decided, expired, or
the transfer failed` is honest but can't tell you which. Check in order:

1. `/pending` — still listed? Then nothing happened; approve again with the
   exact id from the hold message.
2. `/balance` — dropped? The transfer executed (the earlier approve worked).
3. Neither? The request reached a terminal state without moving funds:
   REJECTED, EXPIRED (60-minute window), or **TX_FAILED** — see next section.

> Historical note: before 2026-07-10 the CLI reported **every** approval as
> "no response from agent" / "Transaction Failed", even when the transfer
> completed on-chain (the daemon answers approvals with a bare JSON boolean;
> the CLI collapsed it to an empty string). If an old recording shows that,
> it was a display bug — the FSM and transfers were fine. Fixed in `2748c95`.

---

## Transfer ends TX_FAILED

A held spend the owner approved, or an auto-spend, that executed and failed.
Causes seen live, most common first:

- **Recipient was a bare hex key.** A payment recipient must be the FULL keys
  JSON — `{"nullifier_public_key":"…","viewing_public_key":"…"}` — or an
  `@contact` that resolves to one. A bare 64-char hex is treated as an account
  id *owned by this wallet* (`transfer_private_owned`) and fails for any peer.
- **Recipient keys were corrupted in transit.** Long pastes can lose their
  head to the line editor (below), and key material that travels through the
  LLM gets retyped and mangled. The CLI now blocks keys from reaching the LLM
  and resolves `@contacts` on both paths — use `/send @name …`.
- **Wallet and chain disagree** — see "Balance is 0 / everything fails after
  restarting the sequencer".
- **Sequencer down.** `pgrep -a sequencer_servi` and
  `curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:3040/` (000 = down).

---

## Balance is 0 / transfers fail after restarting the sequencer

**Cause:** the wallet's funds are cryptographic notes bound to specific blocks
on a specific chain. `run-sequencer.sh` (and `run-a2a.sh`, which calls it)
**wipes the chain to a fresh genesis on every start**. Fresh chain + old
wallet = stale notes: balance reads 0, transfers fail, nothing recovers on
its own.

**Rule: the chain and the wallet must be reset together, or neither.**

- Keep the funded wallet → boot the sequencer **without** the wipe:
  ```bash
  cd <logos-execution-zone>
  export RISC0_DEV_MODE=1
  export LOGOS_BLOCKCHAIN_CIRCUITS=$(find /nix/store -maxdepth 1 -name '*logos-blockchain-circuits*' -type d | head -1)
  ./target/release/sequencer_service sequencer/service/configs/debug/sequencer_config.json
  ```
- Fresh start → wipe both: fresh sequencer via `run-sequencer.sh`, delete
  `~/.pilot/wallet_storage.json*`, then `pilot deploy` (or
  `logoscore call pilot initialize ~/.pilot`) to mint and fund a **new
  identity** (the account id changes; old contacts of *your* agent go stale,
  saved contacts of *other* agents stay valid).

---

## Funding or a transfer grinds forever (spinner for ~45 min)

**Cause:** the daemon was started from a shell without `RISC0_DEV_MODE=1` +
`LOGOS_BLOCKCHAIN_CIRCUITS`. The wallet then generates a REAL zk proof per
transfer (~40–45 min on a small box) instead of a dev-mode fake (seconds).

**Fix:** the CLI bakes both into `~/.pilot/.start-daemon.sh` since `f905765`,
and the demo/test scripts export them since `e714537`. If you launch a daemon
by hand, export both first. Real proofs are only for the recorded real-proof
demo (`RUNBOOK.md` §3) — set `RISC0_DEV_MODE=0` deliberately, never by accident.

---

## Pasting keys JSON mangles / goes to the LLM

**Symptom:** you paste `/send {"nullifier_public_key":…} 20 x` and the line
appears clipped, or the agent answers conversationally as if you *chatted*
the keys.

**Cause:** the line editor flushes any input that arrives before the fresh
`> ` prompt is listening — the head of an early paste (including `/send`) is
discarded, and the remainder routes to the LLM as chat. On top of that, the
LLM retypes long hex imperfectly, so keys that travel through it arrive
corrupted.

**Fixes (in the CLI since 93e8ffd):** lines containing key material are never
handed to the LLM (you get a hint instead), and `@contacts` resolve on both
the `/send` and natural-language paths.

**Habits:** paste only when the prompt is idle, and prefer `@contacts` so
there is nothing long to paste (see the owner guide's Contacts section).

---

## Two-agent Docker test fails

- **"Agent B container did not start"** — usually a stale `pilot-agent-b`
  container holding the name (left when Docker Desktop closed before `--rm`
  fired); the `docker run` error was muted, and the printed logs came from
  the OLD container. The script removes corpses first since `e714537`; by
  hand: `docker rm -f pilot-agent-b`.
- **One agent answers "(empty response)" to everything, the other is fine** —
  the silent one is either still replaying a long chain inside `initialize`
  (the script now polls instead of failing) or its pilot module died at boot
  from missing RISC0 env (baked into the script since `e714537`). One silent
  agent then cascades: its partner reports `invalid recipient key: not a hex
  string` because the exchanged key variable was empty — fix the silent
  agent, not the encryption.
- Run it via `bash run-a2a.sh` (fresh short chain, env exported, nwaku
  checked) rather than invoking `test-two-agents-docker.sh` directly against
  a long-lived chain.

---

## LLM chat errors (slash commands still work)

- **`Host api.deepseek.com not found`** — DNS/network blip inside WSL, not a
  pilot bug. Verify with `curl -s -m 10 https://api.deepseek.com/models`;
  retry when it resolves. Slash commands never need the network LLM.
- **Model-id errors** — provider lineups change. Check the provider's live
  model list before trusting old notes (as of 2026-07-11 DeepSeek serves
  `deepseek-v4-flash` and `deepseek-v4-pro`; `deepseek-chat` is gone).
  Update with: `logoscore call pilot metaConfigure llm.model <id>` or re-run
  `pilot deploy`.
- **"command-only mode (no LLM configured)"** — expected on a fresh agent; an
  LLM is attached via `pilot deploy` (re-deploy keeps the same identity).

---

## Demo recording order

`run-a2a.sh` **kills the sequencer and wipes the chain** as its first step —
it destroys the funded wallet every other demo depends on. Film the wallet /
spending-threshold demos first, the two-agent A2A demo last (its agents are
fresh `/tmp` identities that self-fund on the new chain, so it doesn't care).

---

## Quick health checks

```bash
pgrep -a sequencer_servi                       # sequencer up?
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:3040/   # 000 = down
pgrep -f 'logoscore.*-D'                       # daemon up?
tail -20 ~/.pilot/daemon.log                   # syncing? crashed module?
docker ps -a --format '{{.Names}} {{.Status}}' # nwaku up? agent-b corpse?
```

In chat: `/status` (LLM + account + init), `/pending` (held spends),
`/balance` (give it a few seconds after a cold boot).
