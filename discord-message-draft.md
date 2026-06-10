Hi team, I'm building LP-0008 (Autonomous AI Agent Module) and I'm stuck on wallet creation with the demo sequencer. Would appreciate guidance on the correct flow.

**Setup:**
- Demo sequencer via Docker: `ghcr.io/logos-blockchain/logos-blockchain:devnet`
- Running on port 3040 with `SEQUENCER_INITIAL_BALANCE=1000`
- Using `lez_wallet_module` from nix store (logos-lez_wallet_module-module)
- Sequencer is running and responding (HTTP 404 on `/`, block height 0)

**What works:**
- `create_new(config_path, storage_path, password)` → returns 0 (success)
- `create_account_private()` → returns account ID
- `create_account_public()` → returns account ID
- `list_accounts()` → shows created accounts
- `get_current_block_height()` → returns 0

**What fails:**
- `save()` → returns error code 5 (STORAGE_ERROR) — wallet_storage directory stays empty
- `register_public_account(account_id)` → crashes the wallet module process
- `register_private_account(account_id)` → crashes the wallet module process
- `get_balance(account_id, false)` → returns 0 (account never registered on sequencer)
- Sequencer `/accounts` endpoint shows `{"accounts": []}`

**My questions:**
1. What's the correct sequence to create a wallet, register an account on the demo sequencer, and get the initial 1000 token balance?
2. Does `register_public_account` / `register_private_account` require RISC0 proof generation? I tried with `RISC0_DEV_MODE=1` but it still crashes.
3. Is `claim_pinata()` required to get the initial balance, or should `register` be sufficient with `SEQUENCER_INITIAL_BALANCE=1000`?
4. Why does `save()` return STORAGE_ERROR (5) even though the wallet_storage directory exists and is writable?
5. Is there a working example or test that shows the full wallet creation → funding → balance flow with the demo sequencer?

**Environment:**
- WSL2 Ubuntu 22.04
- wallet-ffi header shows `wallet_ffi_init_runtime()` but the function doesn't exist in `libwallet_ffi.so`
- Testnet v0.1.2 Docker image

Thanks for any pointers.
