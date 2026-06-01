# Wallet Funding — Execution Runbook

**Status:** Ready to run when you choose. Nothing here has been executed yet.
**Goal:** give the pilot agent a non-zero on-chain LEZ balance (the last technical gap).
**Prereq:** C: drive should have ~20 GB+ free before starting (build uses ~15 GB temporarily).

See `findings-wallet-funding.md` for the full diagnosis. One-line recap:
> Funding was never blocked by RISC0 proving. The wallet was talking to the wrong
> program — we never ran the LEZ `sequencer_service`. Fix = run the sequencer.

---

## The plan (original full-stack plan, pre-crash)

Build the execution-zone services, run the stack, point the pilot wallet at the
sequencer (`:3040`), fund, verify.

There are **two ways to run the sequencer** — pick one:

| | Full stack (original plan) | Standalone (lighter) |
|---|---|---|
| Services | bedrock (Docker) + indexer + sequencer | sequencer only |
| Docker? | yes | no |
| Proofs | real possible (`RISC0_DEV_MODE=0`) → **demo-grade** | dev-mode (mock L1) |
| Disk/data | heavier (+ ~1–2 GB image) | lighter |
| Build flag | normal build | `--features standalone` |

Use **full stack** for the real demo (real proofs). Use **standalone** for a quick
"does funding work" check. They are **separate builds** — don't do both unless needed.

---

## Key facts (verified from source)

- Sequencer RPC port: **3040** (the wallet's default `sequencer_addr`).
- Circuits version MUST match the pilot's `lez_wallet_module`: pinned rev
  **`d6cf41f66500d4afc157b4f43de0f0d5bfa01443`** = circuits **v0.4.1**.
- Genesis pre-funds these accounts on ANY fresh sequencer (baked into the binary via
  `testnet_initial_state::initial_state()`):
  - public `6iArKUXxhUJqS7kCaPNhwMWt3ro71PDyBj7jwAyE2VQV` = **10000**
    - privkey hex: `10a26a9aec7d34b82364eeae45c5294dbb0a764b000b94eeb9b58511dc487c4d`
  - public `7wHg9sbJwc6h3NP1S9bekfAzB8CHifEcxKswCKUt3YQo` = **20000**
    - privkey hex: `717940b1cc55e5d6b2066dbf1d9a3f26f212f4db08d02388177fcfedd8a9be1b`
  - private `4eGX3M3rgjHsme8n3sSp89af8JRZtYVTesbJjLqaX1VQ` = 10000 (keychain in source)
  - private `3m6HQmCgmAvsxZtxAHPqqEqoBG4335fCG8TzxigyW7rE` = 20000
- Pinata faucet program is registered, but only with `--features testnet` for the
  sovereign claim path.
- Source repo: `~/dev/logos/logos-execution-zone` (WSL).
- Build scripts already written: `~/build-lez.sh` + `~/build-lez-inner.sh`.

---

## Step 1 — Build the services (~15 GB temp disk, ~2 GB download, ~30–45 min)

Uses the nix devShell (stable rust 1.95, overriding the broken 1.94.0 pin) + the
pinned circuits + libclang/openssl. Build script `~/build-lez.sh` does this:

```bash
bash ~/build-lez.sh
# Builds: sequencer_service, indexer_service, wallet (CLI)
# Output: ~/dev/logos/logos-execution-zone/target/release/{sequencer_service,indexer_service,wallet}
```

For the **standalone** variant instead, build with the feature flag:
```bash
cd ~/dev/logos/logos-execution-zone
RUSTUP_TOOLCHAIN=stable cargo build --release --features standalone -p sequencer_service -p wallet
```

**Disk safety:** watch `df -h /mnt/c` in another terminal; if free space nears ~6 GB,
stop the build (Ctrl-C) and reclaim before continuing.

---

## Step 2 — Run the stack

### Full stack (real, Docker)
```bash
# Terminal 1 — bedrock L1 node (Docker; re-pulls ~1-2 GB image first time)
cd ~/dev/logos/logos-execution-zone/bedrock && docker compose up

# Terminal 2 — indexer
cd ~/dev/logos/logos-execution-zone/indexer/service
RUST_LOG=info RISC0_DEV_MODE=1 cargo run --release -p indexer_service configs/indexer_config.json

# Terminal 3 — sequencer (serves wallet on :3040)
cd ~/dev/logos/logos-execution-zone/sequencer/service
RUST_LOG=info RISC0_DEV_MODE=1 cargo run --release -p sequencer_service configs/debug/sequencer_config.json
```

### Standalone (lighter, no Docker)
```bash
cd ~/dev/logos/logos-execution-zone/sequencer/service
RUST_LOG=info RISC0_DEV_MODE=1 cargo run --release --features standalone -p sequencer_service configs/debug/sequencer_config.json
```

Confirm it's up: `curl -s http://127.0.0.1:3040/accounts` (or whatever the RPC exposes).

---

## Step 3 — Validate with the wallet CLI (quick sanity check)

```bash
cd ~/dev/logos/logos-execution-zone
export NSSA_WALLET_HOME_DIR=$(pwd)/wallet/configs/debug
# import a pre-funded genesis account:
./target/release/wallet account import public --private-key 10a26a9aec7d34b82364eeae45c5294dbb0a764b000b94eeb9b58511dc487c4d
# check balance — expect 10000:
./target/release/wallet account get --account-id Public/6iArKUXxhUJqS7kCaPNhwMWt3ro71PDyBj7jwAyE2VQV
```
If this shows 10000, the sequencer + proof path work end-to-end.

---

## Step 4 — Sovereign funding test (the spec-aligned path)

```bash
# create our own account, register it, claim from the faucet:
./target/release/wallet account new public
./target/release/wallet auth-transfer init --account-id Public/<new-id>
./target/release/wallet pinata claim --to Public/<new-id>
./target/release/wallet account get --account-id Public/<new-id>   # expect ~150
```
This verifies (a) the agent can fund itself and (b) the pinata proof-of-work solve
works. NOTE: pinata claim needs the sequencer built `--features testnet` for the
pinata program to exist.

---

## Step 5 — Fund the REAL pilot agent

1. Point the pilot module's wallet config `sequencer_addr` at `http://127.0.0.1:3040`.
2. Either:
   - **Sovereign:** have the agent run `register_public_account` → `claim_pinata`
     (verify the module computes the pinata PoW solution, not the empty `""`), or
   - **Import fallback:** if `lez_wallet_module` exposes an import method, import
     `PRIVATE_KEY_PUB_ACC_A`.
3. Verify non-zero balance in `pilot balance` (CLI) and the Basecamp Wallet view.

**Open question to settle here:** confirm the pilot's `lez_wallet_module` /
`libwallet_ffi.so` was built with circuits **v0.4.1 (rev d6cf41)** — same as the
sequencer — or the agent's proofs will be rejected.

---

## Reclaiming the space afterward

- **Docker image:** `docker system prune` or delete
  `%LOCALAPPDATA%\Docker\wsl\disk\docker_data.vhdx` (instant, frees fully).
- **WSL build scratch:** `rm -rf ~/dev/logos/logos-execution-zone/target` frees ~12–15
  GB inside WSL (keep the small binaries first if you still want them). To return that
  space to C:, shut down WSL (`wsl --shutdown`) and compact the Ubuntu disk
  (admin `diskpart` → `compact vdisk`, or Docker-style: it doesn't auto-shrink).
- The binaries themselves are only a few MB — keep them; delete `target/`.

---

## The three open questions (unverified until step 4–5)

1. **Circuits match** — pilot's `lez_wallet_module` vs the sequencer build (v0.4.1).
2. **Sovereign funding** — does `register` → `claim_pinata` succeed (spec requirement)?
3. **Pinata PoW** — does the module compute the faucet puzzle, or send empty and fail?

---


## UPDATE 2026-06-01 — version match SOLVED; next-session context

**Proven:** the installed pilot `lez_wallet_module` (built from `lssa@9df1217`, March) is
version-incompatible with a May sequencer (RPC methods renamed → "Method not found"). Fix: build
the sequencer at the **March rev**. Done + PROVEN — the real module's `register_public_account`
returned `{"success":true,"tx_hash":"ebf459fd…"}` against the March sequencer on :3040.

### March build/run — STANDALONE (proven, recommended; no Docker)
- Repo `logos-execution-zone` checked out to `9df12170` (detached HEAD; `git checkout main` → back to May `bfdc087`).
- Build: `~/build-lez.sh` (circuits `ec7d298`, nixpkgs `cb369ef2` for libclang, target `cargo build --release --features standalone -p sequencer_runner`).
- Run: `~/run-sequencer.sh` → `target/release/sequencer_runner sequencer_runner/configs/debug` (config is a DIRECTORY). RISC0_DEV_MODE=1, r0vm 3.0.5 on PATH, serves `:3040`.
- Test: `~/wallet-module-test.sh` (run `setup-modules.sh` first after any reboot — `/tmp` wipes).

### March FULL-STACK (alternative — only for maximum realism; heavier, NOT required)
The full stack runs a real L1 chain under the sequencer instead of standalone's mock. Use only if
a reviewer wants a real bedrock chain; standalone is sufficient for funding and the demo.
1. bedrock L1 node via Docker (re-pull `ghcr.io/logos-blockchain/logos-blockchain:devnet`; run `bedrock/` compose).
2. `indexer_service` built from March (`cargo run --release -p indexer_service indexer/.../indexer_config.json`).
3. `sequencer_runner` built WITHOUT `--features standalone`, pointed at bedrock (`node_url localhost:8080`) + indexer.
Cost: Docker image (~1-2 GB) + indexer build + 3 services running. Standalone avoids all of it.

### REMAINING GAP — fund the agent
Pilot code only calls `create_new` / `open` / `create_account_private` / `get_balance` /
`transfer_private` — NO `register`/`claim`. The agent uses a **PRIVATE** account. To fund, either:
- **(A)** add `register_private_account` + `claim_pinata_private_owned` to the pilot deploy flow
  (the sovereign-funding feature — a code addition), or
- **(B)** externally fund the agent's private account after `pilot deploy` (register it, then a
  private pinata claim or a shielded transfer from a genesis account).
Then `pilot balance` + the Basecamp Wallet view show a non-zero balance.

### Real-proof demo (RISC0_DEV_MODE=0)
`spec.md` L125-126 REQUIRES the on-chain segment to run with `RISC0_DEV_MODE=0`, proof generation
visible on camera (dev mode does NOT satisfy it; only the on-chain segment needs it — messaging /
storage / A2A do not, per `decisions.md` L180). Plan: flip the flag to 0 on BOTH sequencer + wallet
and TRY on the laptop first (7.6 GB; LEZ guests are small; lower `RISC0_SEGMENT_PO2` if it runs out
of memory). If it won't fit, run on a 16 GB+ box / cloud VM with a full `rzup install` (proving
keys). Same binaries and flow — only the flag + RAM change.
