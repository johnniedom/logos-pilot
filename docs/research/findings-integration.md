# Integration Findings — 2026-05-20/21

Comprehensive record of everything discovered during Phase E (Integration Verification).

---

## 1. LEZ Network Status

### L1 Testnet (Public, Live)
- **Dashboard:** `https://testnet.blockchain.logos.co/web/`
- **Faucet:** `https://testnet.blockchain.logos.co/web/faucet/`
- **Explorer:** `https://testnet.blockchain.logos.co/web/explorer/`
- **Bootstrap peers:** `65.109.51.37` ports 3000–3003 (quic-v1)
- **Auth required:** Node API endpoints return HTTP 401. Faucet credentials via Discord: `https://discord.com/channels/973324189794697286/1468535289604735038`
- **Latest release:** v0.1.2 (2026-04-13)

### LEZ L2 (No Public Endpoint)
- **Repo:** `github:logos-blockchain/logos-execution-zone` at v0.2.0-rc3 (2026-04-27)
- **Status:** Internal staging only ("stage 2, finalising 100 blocks against latest Bedrock node")
- **No public RPC**, no public faucet, no documented chain ID
- **Architecture:** NOT EVM — custom RISC-V/RISC0 VM with Solana-style account model
- **Default sequencer config:** connects to Bedrock L1 at `http://localhost:18080`, indexer at `ws://localhost:8779`

### L1 Devnet (Internal)
- **Dashboard:** `https://devnet.blockchain.logos.co/web/`
- Auth-gated, intermittently down (502 observed)

---

## 2. Running a Local Sequencer

### Option A: Pre-built Docker Image (Recommended)

Building from source fails — the full LEZ sequencer images are pushed to a **private Docker registry**. Use the pre-built devnet image instead:

```bash
docker pull ghcr.io/logos-blockchain/logos-blockchain:devnet
```

Image contains: `logos-blockchain-demo-sequencer` (7.6 MB), `logos-blockchain-node` (45 MB), `logos-blockchain-faucet` (4.9 MB), demo webapp, ZK circuits.

Run script (`run-sequencer.sh`):
```bash
docker run --rm -p 8080:8080 \
  -e SEQUENCER_LISTEN_ADDR=0.0.0.0:8080 \
  -e SEQUENCER_DB_PATH=/data/sequencer.db \
  -e SEQUENCER_SIGNING_KEY_PATH=/data/sequencer.key \
  -e SEQUENCER_INITIAL_BALANCE=1000 \
  -e SEQUENCER_CHANNEL_ID=6d656d636f696e00000000000000000000000000000000000000000000000001 \
  -v sequencer-data:/data \
  --entrypoint /usr/bin/logos-blockchain-demo-sequencer \
  ghcr.io/logos-blockchain/logos-blockchain:devnet
```

**Required env vars:**
- `SEQUENCER_LISTEN_ADDR` — bind address (0.0.0.0:8080)
- `SEQUENCER_CHANNEL_ID` — hex channel ID (required, panics without it)
- `SEQUENCER_DB_PATH` — SQLite database path
- `SEQUENCER_SIGNING_KEY_PATH` — key file path
- `SEQUENCER_INITIAL_BALANCE` — initial token supply

**Verify:** `curl http://localhost:8080` (empty response = working)

### Option B: Docker Compose from Source (NOT Recommended)

```bash
git clone https://github.com/logos-blockchain/logos-execution-zone
cd logos-execution-zone
docker compose up sequencer_service
```

**Problems encountered:**
1. `explorer_service` fails — Debian trixie apt mirrors return "410 Gone" (DNS/CDN issue in WSL Docker)
2. `sequencer_service` build takes 40+ minutes (Rust + RISC0 toolchain compilation)
3. `cargo chef cook` failed during Rust dependency compilation
4. Full LEZ sequencer images pushed to private registry — can't be pulled

### Docker Caching
- First build is slow (40+ min). Subsequent `docker compose up` starts in seconds (layers cached).
- Same for Nix — first build downloads/compiles, subsequent runs instant from store.
- Docker caches each completed layer. If build fails halfway, re-run resumes from cached layers.

---

## 3. Basecamp Network Configuration

**Basecamp ships with NO default sequencer endpoint.** It is a pure application shell.

### Security Sandbox
- `DenyAllNetworkAccessManager.cpp` — blocks all outgoing HTTP/HTTPS from QML UI apps
- `RestrictedUrlInterceptor.cpp` — blocks non-`qrc:` and non-local-file URLs
- Modules handle networking from their own processes (not sandboxed by Basecamp)

### Wallet Module Configuration
- `lez_wallet_module.open(config_path, storage_path)` reads a `wallet_config.json`
- `lez_wallet_module.create_new(config_path, storage_path, password)` creates new wallet
- Default sequencer addr: `http://127.0.0.1:3040` (only works with local sequencer)
- Wallet UI onboarding prompts user for: config path, storage path, password

### Correct wallet_config.json format (v0.1 / wallet-ffi-0.1.0):
```json
{
    "sequencer_addr": "http://127.0.0.1:8080",
    "seq_poll_timeout": "30s",
    "seq_tx_poll_max_blocks": 15,
    "seq_poll_max_retries": 10,
    "seq_block_poll_max_amount": 100,
    "initial_accounts": []
}
```

**Critical:** `initial_accounts` field is REQUIRED (no `#[serde(default)]`). Omitting it causes "Failed to deserialize wallet config".

### Pre-installed Basecamp Modules
- package_manager_module, package_downloader_module, capability_module
- counter + counter_qml (demos), main_ui, package_manager_ui, webview_app
- LEZ wallet, delivery, storage, chat modules are NOT pre-installed — install via LGX

---

## 4. Real Dependency Modules

All dependency modules exist as separate repos and are buildable via Nix:

| Module | Repo | Build Command |
|--------|------|---------------|
| `lez_wallet_module` | `github:logos-blockchain/logos-execution-zone-module` | `nix build 'github:logos-blockchain/logos-execution-zone-module'` |
| `delivery_module` | `github:logos-co/logos-delivery-module` | `nix build 'github:logos-co/logos-delivery-module'` |
| `storage_module` | `github:logos-co/logos-storage-module` | `nix build 'github:logos-co/logos-storage-module'` |
| `chat_module` | `github:logos-co/logos-chat-module` | `nix build 'github:logos-co/logos-chat-module'` |
| `waku_module` | `github:logos-co/logos-waku-module` | `nix build 'github:logos-co/logos-waku-module'` |

### Key facts:
- All use `logos-module-builder` (except waku_module which has a custom flake)
- None require the sequencer running to build
- `lez_wallet_module` wraps Rust `wallet_ffi` from `logos-blockchain/logos-execution-zone` at `wallet-ffi-0.1.0` ref
- There are TWO wallet modules: `logos-blockchain/logos-execution-zone-module` (REAL, Rust FFI) vs `logos-co/logos-wallet-module` (OLD, Go, "very wip")
- LGX packages: `nix build '<repo>#lgx'` then install via `lgpm install --file <name>.lgx --modules-dir <dir> --allow-unsigned`

### Wallet Module API (confirmed from generated headers):
- `create_new(config_path, storage_path, password)` → int (0 = success)
- `open(config_path, storage_path)` → int
- `create_account_private()` → QString (hex account_id)
- `create_account_public()` → QString
- `get_balance(account_id_hex, is_public)` → QString (JSON)
- `get_private_account_keys(account_id_hex)` → QString (JSON with nullifier_public_key, viewing_public_key)
- `transfer_public(from, to, amount_hex)` → QString (JSON with tx_hash)
- `list_accounts()` → QJsonArray
- `get_sequencer_addr()` → QString
- `save()` → int
- Plus: transfer_shielded, transfer_private, register_public_account, register_private_account, sync_to_block, etc.

### Verified Working (2026-05-21):
```bash
# All 5 real modules load with zero warnings:
logoscore -m /tmp/pilot-logoscore/modules \
  -l lez_wallet_module,delivery_module,storage_module,chat_module,pilot \
  -c "pilot.echo(hello)" --quit-on-finish

# Wallet init + account creation works:
logoscore -m ... -l lez_wallet_module \
  -c "lez_wallet_module.create_new(/tmp/pilot-wallet/wallet_config.json,/tmp/pilot-wallet/storage,password123)" \
  -c "lez_wallet_module.create_account_private()" \
  -c "lez_wallet_module.list_accounts()" \
  --quit-on-finish
# Returns: account_id = 5f0de21c6f8392839c811ea54603b9801a34e44ac4cb6c6922e938a7218a296b
```

---

## 5. LogosAPI / onInit Bug

### Problem
`pilot.initialize()` returns `false` because `logosAPI_` is NULL inside the impl class. The framework never provides the LogosAPI pointer to PilotImpl.

### Root Cause
The code generator (`logos-cpp-generator`) wraps only public methods found in the impl header. Two scenarios, both broken:

**Scenario A — `onInit` IS in the impl header:** The generator wraps it as `void onInit(QVariant api)` which **hides** the base class `LogosProviderBase::virtual void onInit(LogosAPI* api)` (different parameter types = C++ name hiding, not override). The framework calls the base class's empty method, not the generated one.

**Scenario B — `onInit` is NOT in the impl header (our case):** The generator produces NO onInit wrapper at all. The base class's empty `virtual void onInit(LogosAPI* api) {}` runs, and `logosAPI_` is never set. A sed that tries to *replace* `onInit(QVariant)` matches nothing.

### Framework Source (confirmed)

```
/nix/store/z8y8sgiavlkswlsfswmlrq9xd5vc942h-logos-cpp-sdk-headers-0.1.0/include/cpp/logos_provider_object.h
/nix/store/z8y8sgiavlkswlsfswmlrq9xd5vc942h-logos-cpp-sdk-headers-0.1.0/include/cpp/logos_provider_object.cpp
```

`LogosProviderBase` provides:
- `void init(void* apiInstance) override;` — called by framework, stores API, calls virtual onInit
- `virtual void onInit(LogosAPI* api) {}` — override point for derived classes
- `LogosAPI* logosAPI() const;` — protected accessor for stored API

### Fix (VERIFIED WORKING 2026-05-21)

Do NOT put `onInit` in the impl header. Instead, **inject** a new override into the generated glue header via sed in `flake.nix` preConfigure:

```bash
# Inject onInit override before the private: section of the generated provider class
sed -i '/^private:/i\    void onInit(LogosAPI* api) override {\n        m_impl.logosAPI_ = api;\n    }\n' \
  ./generated_code/pilot_qt_glue.h
```

This inserts the override before `private:` in the `PilotProviderObject` class. When the framework calls `LogosProviderBase::init()` → `onInit(LogosAPI*)`, our injected override sets `m_impl.logosAPI_ = api`.

**Key requirements:**
- `logosAPI_` must be **public** on PilotImpl (so the provider can set it directly)
- Do NOT use a setter method — the generator wraps ALL public methods with QVariant conversion
- The old approach (replacing `onInit(QVariant)`) only works if `onInit` is in the impl header, which we don't want

### Inter-Module Call Pattern (correct)
From the `inter-module-comm` skill:

```cpp
// Correct (uses LogosResult):
LogosResult result = logosAPI->callModule("other_module", "methodName", {arg1, arg2});
if (result.success()) {
    QVariant data = result.data();
}

// What we were using (lower-level, also works but different API):
auto* client = logosAPI->getClient("module");
QVariant result = client->invokeRemoteMethod("module", "method", args...);
```

### Wallet Method Names
The wallet module uses **snake_case** method names:
- `create_account_private()` NOT `createAccountPrivate()`
- `get_private_account_keys()` NOT `getPrivateAccountKeys()`
- `get_balance()` NOT `getBalance()`
- `create_new()` NOT `createNew()`

---

## 6. logoscore CLI

### Binary Locations (Nix Store)
```
logoscore:   /nix/store/510yj8rfcjhdijzfj8yhrk8sy7r6k8f5-logos-logoscore-cli-0.1.0/bin/logoscore
logos_host:  /nix/store/90xva89s7bhsi0w6l1p1l44xvpwxz53h-logos-liblogos/bin/logos_host
lgpm:        /nix/store/l2kcbdg9hn7lb053lx111smrvi88jl38-logos-package-manager-cli-1.0.0/bin/lgpm
```

### Inline vs Daemon Mode
- **Inline** (`-c "module.method(args)" --quit-on-finish`): Each invocation is a fresh process. State doesn't persist between separate `-c` calls in separate invocations. Multiple `-c` flags in ONE invocation DO share state.
- **Daemon** (`-D`): Persistent process. Load modules, call methods, state persists. Use for interactive testing.

### Module Installation
- Bare `.so` files don't work — logoscore uses LGX package format
- Install: `lgpm install --file <name>.lgx --modules-dir <dir> --allow-unsigned`
- Creates: `manifest.json` (manifestVersion 0.2.0) + `variant` file + plugin .so

### LOGOS_HOST_PATH
Required environment variable. logoscore spawns each module in a subprocess via `logos_host`. Without it: `[critical] logos_host_qt (or logos_host) not found`.

---

## 7. Key Nix Store Paths

All dependency modules are cached in the Nix store after first build:

| Module | Nix Store Path |
|--------|---------------|
| `lez_wallet_module` | `/nix/store/rqcw1pvmgyzs4f4fz4m6xxhri1v0gkc9-logos-lez_wallet_module-module/` |
| `delivery_module` | cached (via logos-delivery-module build) |
| `storage_module` | cached (via logos-storage-module build) |
| `chat_module` | cached (via logos-chat-module build) |
| `logos-cpp-sdk-generator` | `/nix/store/6sx0c8lccj5bax7b3ak4nzfhd6cswvsr-logos-cpp-sdk-generator-0.1.0/bin/logos-cpp-generator` |
| `logos-cpp-sdk-headers` | `/nix/store/z8y8sgiavlkswlsfswmlrq9xd5vc942h-logos-cpp-sdk-headers-0.1.0/include/cpp/` |

---

## 8. Capability Module (Required for Inter-Module Calls)

### Problem
When pilot calls `logosAPI->getClient("lez_wallet_module")->invokeRemoteMethod(...)`, the framework first requests an auth token from `capability_module.requestModule()`. Without a real capability_module, this call fails with "Method call returned invalid result".

### Root Cause
Our original setup created a stub `capability_module` directory with only `manifest.json` and `variant` — no actual `.so` plugin. The module "loaded" (logoscore created a registry for it) but couldn't handle method calls.

### Fix
Install the **real** `capability_module` from the Nix store:

```bash
CAP_LGX=$(find /nix/store -maxdepth 2 -name "logos-capability_module-module-lib.lgx" 2>/dev/null | head -1)
lgpm install --file "$CAP_LGX" --modules-dir /tmp/pilot-logoscore/modules --allow-unsigned
```

The real capability_module is built by `logos-module-builder` and cached in the Nix store. It handles auth token exchange between modules — every `invokeRemoteMethod` call goes through it.

### Module Load Order
`capability_module` must be loaded **first** so it's available when other modules initialize:

```bash
logoscore -m /tmp/pilot-logoscore/modules \
  -l capability_module,lez_wallet_module,delivery_module,storage_module,chat_module,pilot \
  -c "pilot.initialize(/tmp/pilot-data)" --quit-on-finish
```

---

## 9. Data Directory Must Exist Before SQLite Open

`sqlite3_open()` does NOT create parent directories. If the data directory doesn't exist, `initDatabase()` throws and crashes the module process.

Fix: call `mkdir(dataDir.c_str(), 0755)` before `sqlite3_open()`. No error check needed — `mkdir` returns -1 if the directory already exists, which is harmless.

---

## 10. Verified Working Flow (2026-05-21)

### Full end-to-end initialization confirmed:

```bash
export LOGOS_HOST_PATH=/nix/store/90xva89s7bhsi0w6l1p1l44xvpwxz53h-logos-liblogos/bin/logos_host
logoscore -m /tmp/pilot-logoscore/modules \
  -l capability_module,lez_wallet_module,delivery_module,storage_module,chat_module,pilot \
  -c "pilot.initialize(/tmp/pilot-data)" \
  -c "pilot.metaStatus()" \
  -c "pilot.metaSkills()" \
  --quit-on-finish
```

### Results:

| Step | Result |
|------|--------|
| Module load (6 modules) | All loaded with zero warnings |
| `logosAPI_` | **SET** (onInit override injection works) |
| `initDatabase` | SQLite WAL database created at `/tmp/pilot-data/pilot.db` |
| `initWallet` | `create_new` returned 0 (success) |
| `create_account_private` | Account ID: `41a7876b...` |
| `get_private_account_keys` | Real NPK (nullifier + viewing public keys) |
| `pilot.initialize()` | **`true`** |
| `pilot.metaStatus()` | Full status with identity, balance, skills |
| `pilot.metaSkills()` | 21 skills across 6 categories |
| Balance query | `get_balance` returns `"0"` (correct — no sequencer funding) |

### Identity Created (via wallet-ffi KeyChain::new_os_random):
- **Account ID:** `41a7876b0d1a330d4f61f514fb63b454da8f4ef9c02682d4b224ada68e2c8f83`
- **Nullifier Public Key:** `72246aacf61680155613259032818c08725435136983695d12241efb5439f2cf`
- **Viewing Public Key:** `0230d6a870e141756c28896a58ebe61e833508debf590ee276d6e10ba46c4b0c29`

### Docker Sequencer NOT Required For:
- Wallet creation (`create_new`)
- Account generation (`create_account_private`)
- Key retrieval (`get_private_account_keys`)
- Local balance queries (`get_balance` returns 0)

### Docker Sequencer Required For:
- Funding accounts (need faucet or transfer)
- Sending transactions (`transfer_public`, `transfer_shielded`)
- Syncing to chain state (`sync_to_block`)

---

## 11. Setup Scripts

| Script | Purpose |
|--------|---------|
| `run-sequencer.sh` | Start local LEZ sequencer via Docker |
| `setup-real-modules.sh` | Install all 6 real modules (including capability_module) via lgpm |
| `test-initialize.sh` | Test pilot.initialize() |
| `test-wallet.sh` | Test wallet module directly |
| `test-debug-init.sh` | Test with wallet pre-init + debug output |

---

## 12. Bugs Fixed Summary

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| `logosAPI_` always NULL | Generator doesn't produce `onInit` when absent from header; old sed replaced nothing | Inject new `onInit(LogosAPI*)` override via `sed -i '/^private:/i\...'` |
| `capability_module` call fails | Stub had manifest only, no `.so` plugin | Install real capability_module from Nix store LGX |
| `Failed to open pilot database` | `sqlite3_open` doesn't create parent dirs | Add `mkdir(dataDir.c_str(), 0755)` before open |
| `method not found: "getBalance"` | Wallet uses snake_case method names | Changed to `get_balance` |
| Wallet config deserialization fail | Missing `initial_accounts` field (serde requires it) | Added `"initial_accounts": []` to config |
| Wrong wallet method names | camelCase vs snake_case | `createAccountPrivate` → `create_account_private`, etc. |
