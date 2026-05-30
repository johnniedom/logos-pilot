# Wallet Funding & RISC0 Proving — Deep Investigation

**Date:** 2026-05-30
**Status:** ROOT CAUSE CORRECTED — see the box below. The earlier "RISC0 proving is
broken" conclusion was **wrong**; the real cause is an **incomplete devnet topology**.

This document captures the complete investigation into why a freshly-created agent
wallet shows a `0` balance and cannot be funded on the local LEZ demo sequencer.

---

## ✅ CORRECTED ROOT CAUSE (2026-05-30, later session — supersedes the TL;DR below)

**We were never running the LEZ sequencer at all.** Funding was not blocked by RISC0
proving. It was blocked because the wallet was talking to the **wrong service**.

The intended LEZ devnet is a **four-service stack** (confirmed from
`logos-execution-zone/README.md` + `Justfile` + `configs/`):

| Service | Role | Port | How to run |
|---------|------|------|------------|
| **bedrock node** (`logos-blockchain`) | L1 consensus / DA | 8080 → 18080 | `just run-bedrock` (docker) |
| **indexer_service** | block/tx index | ws 8779 | `just run-indexer` |
| **sequencer_service** (execution-zone) | **the LEZ sequencer the wallet talks to** | **3040** | `just run-sequencer` |
| explorer_service | block explorer UI | 8080 web | `just run-explorer` (optional) |

What we actually had running: **only the bedrock node**, started with the wrong
entrypoint (`logos-blockchain-demo-sequencer`) and exposed on host `8080`. We then
pointed the wallet's `sequencer_addr` at `8080`. So:

- The wallet submitted its (correctly generated) proofs to the **bedrock L1 node**, not
  to `sequencer_service`. The bedrock node does not accept NSSA wallet transactions the
  way `sequencer_service` does → `register` ran the full proof (~23 s) then
  `METHOD_FAILED`, and `/accounts` on the sequencer stayed empty.
- `create_account` worked only because it is local (no network).
- `get_current_block_height` returned `0` because the bedrock node *does* answer that
  read — which misled us into thinking the right sequencer was reachable.

### Two corrections to the old conclusions

1. **Dev-mode proofs are the intended dev path, not "invalid".** `just run-sequencer`
   runs `sequencer_service` with **`RISC0_DEV_MODE=1`**. When the *sequencer* is also in
   dev mode, it accepts dev-mode receipts. The "dev mode … to produce valid proofs"
   warning only means *not valid against a real chain* — on the matching dev sequencer
   they verify fine. The wallet and the sequencer must **both** be in dev mode (or both
   in real mode). Our wallet was in dev mode but it was hitting the bedrock node, not a
   dev-mode sequencer.

2. **The genesis pre-funds accounts whose private keys we have.** Every
   `sequencer_config.json` carries a `genesis` array of pre-funded supply accounts:
   - `configs/docker-all-in-one/sequencer_config.json` funds
     `6iArKUXxhUJqS7kCaPNhwMWt3ro71PDyBj7jwAyE2VQV` (10000) and three others. The
     **private signing keys** for these are hardcoded in
     `testnet_initial_state/src/lib.rs` (`PRIVATE_KEY_PUB_ACC_A/B`, plus full keychains
     for the private accounts).
   - `sequencer/service/configs/debug/sequencer_config.json` funds a *different* set
     (`CbgR6tj5…`, `2RHZhw9h…`, …).

   → Whichever config the sequencer is launched with determines the funded accounts. To
   get an instantly-funded agent wallet, run the sequencer with the
   **docker-all-in-one** genesis and **import** `PRIVATE_KEY_PUB_ACC_A` into the agent's
   wallet (wallet-ffi `add_imported_public_account`). No register/claim/proof needed for
   a non-zero balance.

### The fix (two viable routes)

- **Route A — register/claim against the real sequencer (matches the spec's
  sovereign-funding story).** Stand up bedrock + indexer + `sequencer_service` (all with
  `RISC0_DEV_MODE=1`), point the pilot wallet at `sequencer_service` (`:3040`), then the
  agent's own `register_public_account` → `claim_pinata` succeed because the dev
  sequencer accepts dev-mode proofs. Note `claim_pinata` also needs the **proof-of-work
  solution** (SHA-256 brute force, computed by the CLI in `find_solution`); confirm the
  module computes it internally rather than relying on the empty `""` arg.
- **Route B — import a genesis account (fastest non-zero balance).** Run the sequencer
  with the docker-all-in-one genesis and import `PRIVATE_KEY_PUB_ACC_A` (account
  `6iArK…`, balance 10000) into the agent wallet. Instant funds; spends still need the
  sequencer up (in dev mode) for their transfer proofs.

Build note: both routes require building the execution-zone Rust workspace
(`sequencer_service` + `indexer_service`). Toolchain pin is `1.94.0` (the local copy is
broken — missing manifest from a failed rustup download); `stable` is installed. Either
repair `1.94.0` or build via `docker compose up` / the nix flake. RISC0 circuits come
from `./scripts/setup-logos-blockchain-circuits.sh` or the flake's
`LOGOS_BLOCKCHAIN_CIRCUITS` env var.

---

## TL;DR (SUPERSEDED — kept for the investigation trail; see corrected root cause above)

The agent's wallet can **create accounts** but cannot **fund them**. Funding requires
submitting a transaction (`register_public_account` / `claim_pinata`) to the sequencer,
and every transaction on LEZ must carry a **RISC0 zero-knowledge proof**.

- The RISC0 prover is **fully embedded** in `libwallet_ffi.so` (≈100 MB) — no guest
  binary is missing.
- With **real proofs** (`RISC0_DEV_MODE` unset), `register_public_account` runs for
  ~23 s then returns `METHOD_FAILED`. The transaction never reaches the sequencer.
- With **dev-mode proofs** (`RISC0_DEV_MODE=1`), `claim_pinata` returns `ok` instantly,
  but the proof is invalid (the library itself warns "dev mode … to produce valid
  proofs"), so the sequencer would reject it. The account still never appears on-chain.
- The wallet-ffi swallows its own internal error — even `RUST_LOG=debug` /
  `RUST_BACKTRACE=1` produce nothing in the daemon log — so the exact failure reason
  inside the proving/submission path is not externally observable.

> **Note (corrected):** the "blocker is the RISC0 proving step" conclusion is wrong.
> The proof generated fine; it was submitted to the bedrock node instead of
> `sequencer_service`. See the corrected root cause above.

---

## What works (verified)

| Operation | Result | Notes |
|-----------|--------|-------|
| `create_new(config, storage, password)` | `0` | Wallet handle created |
| `create_account_public()` | account ID | e.g. `cb2836e1…2fe1b9` |
| `create_account_private()` | account ID | Private/shielded account |
| `list_accounts()` | array | Shows all created accounts |
| `get_current_block_height()` | `0` | **Proves sequencer is reachable** |
| `account_id_to_base58` / `from_base58` | works | Encoding helpers |

These are all **local** operations (key generation, storage, encoding) or read-only
queries. None require a proof, which is why they always succeed.

## What fails (verified)

| Operation | RISC0_DEV_MODE | Behaviour |
|-----------|----------------|-----------|
| `register_public_account(id)` | unset (real) | Runs ~23 s → `METHOD_FAILED`, nothing on sequencer |
| `register_public_account(id)` | `1` (dev) | Hangs → replica timeout (`RPC_FAILED`) |
| `register_private_account(id)` | either | Same as public |
| `claim_pinata(pinata, winner, "")` | `1` (dev) | Returns `ok` instantly, but no on-chain effect |
| `claim_pinata(pinata, winner, "")` | unset (real) | Fails like register |
| `save()` | — | Returns `5` = `STORAGE_ERROR`; `wallet_storage/` stays empty |

---

## The correct flow (from official docs)

Per the Logos docs the funding sequence is:

1. `wallet account new public` — create account
2. `wallet auth-transfer init --account-id <id>` — **initialise the account under the
   authenticated-transfer program** (registers it on-chain)
3. `wallet pinata claim --to <id>` — claim 150 tokens from the Piñata faucet
4. `wallet account get --account-id <id>` — balance now shows 150

Docs:
- https://github.com/logos-co/logos-docs/blob/main/docs/lez/get-started/quickstart-for-the-logos-execution-zone-wallet.md
- https://github.com/logos-co/logos-docs/blob/main/docs/lez/transfer-tokens/transfer-native-tokens-on-the-logos-execution-zone.md

### Module-API mapping

We call the `lez_wallet_module` (Logos Core module) via Qt Remote Objects, **not** the
standalone `wallet` CLI. The module exposes:

```
create_new, open, save,
create_account_public, create_account_private,
register_public_account, register_private_account,   ← believed = "auth-transfer init"
claim_pinata, claim_pinata_private_owned_*,           ← = "pinata claim"
get_balance, list_accounts, get_*_account_keys,
transfer_public/private/shielded/deshielded/*,
sync_to_block, get_current_block_height, ...
```

There is **no `auth_transfer_init` method** — `register_public_account` is the closest
equivalent and is almost certainly the same on-chain initialisation step.

Pinata program account (base58, from `logos-execution-zone/common/src/lib.rs`):
```
EfQhKQAkX2FJiwNii2WFQsGndjvF1Mzd7RuVe7QdPLw7
```

---

## Why it actually fails: RISC0 proving

LEZ is a privacy chain. Every state-changing transaction is proven off-chain with a
**RISC0 zkVM proof** and the proof (not the raw tx) is submitted to the sequencer.

- `create_account` = local only → no proof → always works.
- `register` / `claim_pinata` = on-chain tx → requires a proof → this is where it breaks.

### The prover is embedded, not missing

`libwallet_ffi.so` is **≈100 MB**. A plain wallet would be a few MB. Strings analysis
confirms the entire RISC0 stack is compiled in:

```
risc0-zkvm-3.0.5
risc0_circuit_rv32im_cpu_accum / _witgen
risc0_circuit_keccak_cpu_witgen
risc0_circuit_recursion_cpu_eval_check
"Proving lift…", "Proving join…", "Proving resolve…"
"zkVM: dev mode is disabled. Unset RISC0_DEV_MODE … to produce valid proofs"
```

So **Possibility "missing guest binary" is ruled out.** The prover and circuits ship
inside the library.

### Dev mode vs real mode

| Mode | Speed | Validity | Observed |
|------|-------|----------|----------|
| `RISC0_DEV_MODE=1` | instant | **invalid** (fake receipt) | `claim_pinata` → `ok` fast, but sequencer would reject |
| unset / `=0` | ~20–30 s/proof | valid | `register` runs 23 s then `METHOD_FAILED` |

The embedded warning string — *"dev mode is disabled. Unset RISC0_DEV_MODE … to produce
valid proofs"* — confirms dev-mode receipts are not chain-valid. So dev mode can never
produce a really-funded account; it only makes the local call return quickly.

### Environment-variable propagation (a real fix we found)

Each Logos module runs in its **own spawned `logos_host` child process**. An env var
exported in the shell that launches the CLI does **not** reach the module unless the
**daemon** is started with it (children inherit the daemon's environment).

Verified: starting the daemon with `RISC0_DEV_MODE=1` puts it in the wallet process:
```
PID <wallet> (name lez_wallet_module): RISC0_DEV_MODE=1
```
After this, `claim_pinata` stopped crashing and returned `ok` (dev-mode fast path).
This is the mechanism to control RISC0 mode for the wallet module — but note dev-mode
proofs are not chain-valid.

---

## The remaining unknown

With **real proofs**, `register_public_account`:
- runs ~23 s (consistent with genuine CPU proof generation),
- returns `METHOD_FAILED` (the method executed and failed — it did **not** crash and
  did **not** hit the 5-minute timeout),
- the sequencer receives nothing (`/accounts` stays `{"accounts":[]}`),
- and the wallet-ffi's internal error is **not surfaced** to the daemon log, even with
  `RUST_LOG=debug` and `RUST_BACKTRACE=1`.

So the proof either fails to generate, fails to verify, or fails to submit — and we
cannot see which from outside the library. **This is the one question for the Logos
team:** what does `register_public_account` need to succeed against the demo sequencer
(guest/circuit env, prover backend, an auth-transfer init step we're missing, or a
sequencer-side requirement), and how should `RISC0_DEV_MODE` be set for a *valid*
on-chain registration?

---

## Reproduction

```bash
# Sequencer (demo) — note the port; wallet_config.json must match it
docker run --rm -p 8080:8080 \
  -e SEQUENCER_LISTEN_ADDR=0.0.0.0:8080 \
  -e SEQUENCER_DB_PATH=/data/sequencer.db \
  -e SEQUENCER_SIGNING_KEY_PATH=/data/sequencer.key \
  -e SEQUENCER_INITIAL_BALANCE=1000 \
  -e SEQUENCER_CHANNEL_ID=6d656d636f696e…0001 \
  -v sequencer-data:/data \
  --entrypoint /usr/bin/logos-blockchain-demo-sequencer \
  ghcr.io/logos-blockchain/logos-blockchain:devnet

# Daemon must be started WITH RISC0 env so the wallet child inherits it
export RISC0_DEV_MODE=1   # or unset for real proofs
logoscore --config-dir <cfg> -D -m <modules> &

logoscore … load-module capability_module
logoscore … load-module lez_wallet_module
logoscore … call lez_wallet_module create_new <config> <storage> <password>
logoscore … call lez_wallet_module create_account_public            # → account id
logoscore … call lez_wallet_module register_public_account <id>     # ← fails here
logoscore … call lez_wallet_module claim_pinata <PINATA> <id> ""
logoscore … call lez_wallet_module get_balance <id> true            # → 0
curl -s http://127.0.0.1:8080/accounts                              # → {"accounts":[]}
```

---

## Impact on the submission

Everything **upstream and downstream of token balance** works:

- Identity / keypair generation (NPK/ISK via `create_account_private`)
- Owner channel, encrypted messaging, A2A discovery + tasking
- Encrypted storage (upload/encrypt/store/download/decrypt)
- Spending FSM logic (threshold hold/approve/reject — exercised with mocked balances)
- 21 skills, CLI + Basecamp UI

The only gap is a **non-zero on-chain balance on the local demo sequencer**, blocked by
the RISC0 proving step for `register`/`claim_pinata`. This is a wallet-ffi / RISC0
integration question, not an agent-module bug.

## Environment

- Ubuntu 22.04 on WSL2
- `libwallet_ffi.so` ≈100 MB, RISC0 zkVM 3.0.5 embedded
- `RISC0_DEV_MODE` honored only when set on the **daemon** (children inherit)
- Demo sequencer: `ghcr.io/logos-blockchain/logos-blockchain:devnet`, testnet v0.1.2
