# Next-session task: add sovereign self-funding to the pilot (decided: option A)

## Goal
On deploy, the agent funds **itself**: after creating its private account, it **registers** that
account and **claims from the pinata faucet**, so `pilot balance` + Basecamp show a non-zero
balance. Satisfies the spec's sovereign-funding requirement. No agent rename — keep dep name
`lez_wallet_module`.

## Proven context (don't re-investigate)
- Version match is SOLVED: build the sequencer at the MARCH rev (`logos-execution-zone @ 9df12170`).
  The installed `lez_wallet_module` (lssa@9df1217, March) is compatible with it; a May sequencer is
  NOT (RPC renamed → "Method not found").
- Real module `register_public_account` returned `{"success":true,"tx_hash":...}` against the March
  sequencer on :3040 — confirmed working.
- Start sequencer: `~/run-sequencer.sh` (March `sequencer_runner sequencer_runner/configs/debug`,
  RISC0_DEV_MODE=1, r0vm 3.0.5 on PATH, :3040).
- After reboot: open Docker if WSL has no internet; `setup-modules.sh` (re-installs 6 modules; /tmp wipes).
- Agent account is **PRIVATE** (`create_account_private`) → use the **private** funding methods.

## Where to add it
`pilot-module/src/pilot_identity.cpp`, in `initWallet()`, right after `create_account_private`
(sets `agentAccountId_`). Make it idempotent (store a "funded" flag in pilot.db; skip if set).

## Call sequence (via logosAPI_->callModule("lez_wallet_module", ...))
1. `register_private_account(agentAccountId_)` — private auth-transfer init. Returns a SECRET that
   must be decoded into local state (CLI: `decode_insert_privacy_preserving_transaction_results`).
   If the module doesn't do this internally, balance reads 0 until decoded/synced.
2. Compute the pinata PoW solution: read the pinata account's 33-byte data via `get_account`,
   brute-force SHA-256 until N leading zero bytes (CLI `find_solution`). Check if the module's
   claim computes it internally first; if it takes a solution arg, compute it here.
3. `claim_pinata_private_owned(pinata_id, winner_id=agentAccountId_, solution)`.
   Pinata account: `EfQhKQAkX2FJiwNii2WFQsGndjvF1Mzd7RuVe7QdPLw7`.
4. Sync private state so `get_balance` reflects it (CLI `account sync-private`; module likely
   `sync_to_block` / a sync method).

## MUST verify exact method names/signatures (not extractable from binary)
Confirm from module source `github:logos-blockchain/logos-execution-zone-module@62829623` or the
CLI facades in `~/dev/logos/logos-execution-zone/wallet/src/cli/programs/`. Likely names:
`register_private_account`, `claim_pinata_private_owned` (maybe a suffix). CLI facade names map 1:1.

## Reference template (the working private flow)
`~/dev/logos/logos-execution-zone/wallet/src/cli/programs/`:
- `native_token_transfer.rs` → `Init` (private branch): register-private + secret decode.
- `pinata.rs` → `ClaimPrivateOwned`: PoW solve + private claim + decode + persist.
Mirror that order in C++.

## Build / test loop
1. Edit `pilot_identity.cpp`.
2. Rebuild pilot module (universal C++ → regenerate + `nix build .#lgx`, see project CLAUDE.md),
   then `setup-modules.sh`.
3. `~/run-sequencer.sh` → deploy pilot → `pilot balance` should show ~150.
- Env: RISC0_DEV_MODE=1 for dev testing; daemon must have r0vm on PATH + RISC0_DEV_MODE so the
  module child inherits it.

## Constraints (project CLAUDE.md)
Pure C++ in pilot_impl.h (no Qt types). Inter-module via callModule. Spending FSM untouched
(this is funding). Identity via KeyChain::new_os_random (already done).

## Done when
Fresh deploy → agent auto-registers + claims → `pilot balance` and Basecamp Wallet both show
~150 LEZ, no manual steps. (Real-proof `RISC0_DEV_MODE=0` demo is a separate task — try laptop
first, lower RISC0_SEGMENT_PO2 if OOM, else 16GB box; both sides flag=0 + full rzup install.)
