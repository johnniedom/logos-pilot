Thanks! Went through both docs. I'm not using the standalone `wallet` CLI — I'm calling the **lez_wallet_module** (Logos Core module) via Qt Remote Objects from my agent module.

Here's exactly what I've narrowed it down to:

**Works fine:**
- `create_new` → 0
- `create_account_public` / `create_account_private` → returns account ID
- `list_accounts` → shows accounts
- `get_current_block_height` → 0 (reaches sequencer)

**Hangs ~20s then the replica times out (module becomes unreachable):**
- `register_public_account(id)`
- `register_private_account(id)`
- `claim_pinata(pinata_id, winner_id, "")`

The capability_module logs `Timeout waiting for replica: "lez_wallet_module"` — the module doesn't crash on load (loads fine standalone), it hangs specifically on these sequencer-submitting methods.

My theory: these methods need RISC0 proof generation. I set `RISC0_DEV_MODE=1` but each module runs in its own spawned `logos_host` child process, so the env var doesn't reach the wallet module. So it tries real proving and hangs.

Questions:
1. Is that the issue — do register/claim_pinata require RISC0 proving, and is there a guest binary that needs to be present?
2. How do I get `RISC0_DEV_MODE` into the module's child process, or is there a config flag for the lez_wallet_module?
3. Is `register_public_account` the module equivalent of the CLI's `auth-transfer init`? (I don't see an auth-transfer method in the module API.)

Setup: Ubuntu 22.04 / WSL2, devnet docker sequencer on :3040 with INITIAL_BALANCE.
Repo: [link]
