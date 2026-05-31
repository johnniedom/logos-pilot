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
