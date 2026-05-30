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

### Port Configuration (Critical)

The `lez_wallet_module`'s wallet-ffi library has a hardcoded default sequencer address of `http://127.0.0.1:3040`. When `create_new` is called to create a wallet, wallet-ffi **overwrites** the config file with this default — regardless of what was written to the file beforehand.

Our sequencer runs on port **8080**. This mismatch causes silent wallet failures on fresh deploys: the wallet is created successfully, but every subsequent operation talks to port 3040 where nothing is listening.

**Fix (implemented in pilot_identity.cpp):** After calling `create_new`, immediately rewrite the config file with the correct `sequencer_addr`, then call `open` to reconnect with the right address.

**For users on different ports:** Set `PILOT_SEQUENCER_ADDR` to match your sequencer:
```bash
export PILOT_SEQUENCER_ADDR=http://127.0.0.1:9000  # or whatever port
./pilot-cli/result/bin/pilot deploy
```

The pilot module reads this env variable and writes it to the wallet config after every `create_new` call, ensuring the wallet-ffi default never takes effect.

**Why this only affects fresh deploys:** On subsequent runs, `initWallet()` calls `open` first (which succeeds because the wallet already exists), skipping `create_new` entirely. The config overwrite only happens during `create_new`.

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

## 12. Chat Module Limitation — callMethod Dispatch Empty

### Problem
The `chat_module` loads and initializes, but `ChatModuleProviderObject::callMethod` rejects ALL method names with "unknown method". Methods like `createIntroBundle`, `sendMessage`, `newPrivateConversation` exist in the `ChatModuleImpl` C++ class (confirmed via binary symbols) but are not wired into the callMethod dispatch.

### Root Cause
The chat module is a **new-style LogosProviderPlugin** (like pilot) with its own `ChatModuleProviderObject`. Unlike the wallet/storage/delivery modules which are **old-style Qt plugins** wrapped by `QtProviderObject` (auto-dispatches via Qt's meta-object system), the chat module's dispatch is manually implemented — and it's empty.

The underlying Rust FFI (`liblogoschat`) is fully implemented with async callback pattern:
- `chat_create_intro_bundle(ctx, callback, userData)`
- `chat_send_message(ctx, callback, userData, convoId, contentHex)`
- `chat_new_private_conversation(ctx, callback, userData, introBundleStr, contentHex)`

But the C++ provider never wired these into callMethod.

### Module Type Comparison

| Module | Plugin Type | callMethod | Status |
|--------|-----------|------------|--------|
| `lez_wallet_module` | Qt QObject → `QtProviderObject` wraps | Auto-dispatch via Qt MOC | **All methods work** |
| `storage_module` | Qt QObject → `QtProviderObject` wraps | Auto-dispatch via Qt MOC | **All methods work** |
| `delivery_module` | Qt QObject → `QtProviderObject` wraps | Auto-dispatch via Qt MOC | **All methods work** |
| `chat_module` | LogosProviderPlugin → custom provider | Manual dispatch (empty) | **No methods work** |
| `capability_module` | LogosProviderPlugin → custom provider | Manual dispatch (implemented) | **Works** |
| `pilot` | LogosProviderPlugin → custom provider | Auto-generated dispatch | **Works** |

### Event API (Not Usable from Other Modules)
The generated `ChatModule` client API uses events (`on()`/`trigger()`), but events emitted ON the chat module's remote object via IPC are not received by the `ChatModuleProviderObject` — it has no event handler wired on the provider side. Events work for in-process UI apps (QML) but not for cross-process module-to-module communication.

---

## 13. Delivery Module as Transport Layer — The Solution

### Discovery
After finding that the chat module's API is inaccessible from other modules, we discovered that `delivery_module` (Qt-wrapped, auto-dispatch) provides the actual Waku messaging transport:

```cpp
// All methods auto-dispatch via QtProviderObject — they WORK
LogosResult createNode(const QString& cfg);
LogosResult start();
LogosResult stop();
LogosResult send(const QString& contentTopic, const QString& payload);
LogosResult subscribe(const QString& contentTopic);
LogosResult unsubscribe(const QString& contentTopic);
LogosResult connect(const QString& peerId, const QStringList& peerAddresses);
```

### Waku Network Configuration (CRITICAL)

The delivery module needs matching cluster/shard config to connect to the Logos relay network:

```json
{
  "clusterId": 2,
  "shards": [0, 1, 2, 3, 4, 5, 6, 7],
  "staticNodes": ["/ip4/127.0.0.1/tcp/30303/p2p/16Uiu2HAmQLoBHmX5KhAQREhLcrVRZsdXkV55ue9x58xXuv4SzFgx"]
}
```

- **`clusterId: 2`** — must match nwaku Docker config (`--cluster-id=2`)
- **`shards: [0..7]`** — must match nwaku's `--num-shards-in-network=8`
- **AutoSharding required** — without clusterId+shards, subscribe fails: `"SubscriptionManager requires AutoSharding"`
- **`"nat": "none"`** is invalid for delivery (Waku) — only valid for storage (Codex)

### Waku Content Topic Format

Must be exactly 4 segments: `/<application>/<version>/<topic-name>/<encoding>`

```
/pilot/1/owner-<accountId>/proto     ✓ (4 segments)
/pilot/1/inbox-<npk>/proto           ✓ (4 segments)
/pilot/1/group-<groupId>/proto       ✓ (4 segments)
/pilot/1/discovery/proto             ✓ (4 segments)
/pilot/1/discovery/topic/proto       ✗ (5 segments — "generation should be numeric")
/pilot/1/discovery-topic/proto       ✓ (4 segments — encode filter in topic name)
```

### Verified: Connected to Real Logos Network

```
Dialing multiple peers... numOfPeers=7
Dial successful: 16U*NSkuby (delivery-01.do-ams3.logos.dev.status.im)
Dial successful: 16U*VpJprH (delivery-02.do-ams3.logos.dev.status.im)
Dial successful: 16U*P1S397 (delivery-01.gc-us-central1-a.logos.dev.status.im)
Dial successful: 16U*TwqBiH (delivery-02.gc-us-central1-a.logos.dev.status.im)
Dial successful: 16U*kGyuEP (delivery-01.ac-cn-hongkong-c.logos.dev.status.im)
Dial successful: 16U*T3UNSE (delivery-02.ac-cn-hongkong-c.logos.dev.status.im)
successfulConns=6 attempted=7
connectionStatus: Connected
```

Six relay nodes across Amsterdam, US-Central, and Hong Kong. Messages propagated to all peers.

---

## 14. Storage Module Configuration

### NAT Configuration
The storage module (Codex-based) uses libp2p, not Waku. UPnP/NAT-PMP is not available in WSL, causing 20-second timeouts.

Fix: pass `{"nat":"none"}` to `storage_module.init()`. This disables NAT traversal and the storage node starts instantly.

### LogosResult Extraction
Storage methods (`uploadInit`, `uploadFinalize`, `downloadChunks`) return `LogosResult`, a custom Qt type with `{success, value, error}` fields. It serializes as a registered QVariant type through IPC, NOT as a QVariantMap.

```cpp
// WRONG — returns empty string
QString sessionId = result.toString();

// CORRECT — extract from LogosResult
if (result.canConvert<LogosResult>()) {
    LogosResult lr = result.value<LogosResult>();
    if (lr.success)
        sessionId = lr.value.toString();
}
```

### Full Upload Pipeline (Verified)
```
storageUpload(path, label)
  → generateFileKey()           // AES-256-GCM key
  → aesEncrypt(plaintext, key)  // encrypt file content
  → uploadInit(label)           // returns session ID via LogosResult
  → uploadChunk(sessionId, encryptedData)
  → uploadFinalize(sessionId)   // returns CID via LogosResult
  → store CID + label + key in SQLite
  → return {"cid": "zDvZRwzm...", "label": "...", "encrypted": true}
```

---

## 15. ECIES Encryption

### Agent ECIES Keypair
The agent generates a secp256k1 ECIES keypair during `createIdentity()`, separate from the wallet keys (wallet-ffi only exposes public keys, not private).

- **Public key** (`agentEciesPub_`): published in agent card, used by others to encrypt messages TO the agent
- **Private key** (`agentEciesPriv_`): stored in SQLite config, used to decrypt incoming messages
- Both keys persist across restarts via `loadIdentity()`

### Owner Channel Encryption
- **Agent → Owner**: `eciesEncrypt(ownerNpk_, message)` — only owner can decrypt
- **Owner → Agent**: encrypt with `agentEciesPub_` — agent decrypts with `agentEciesPriv_`
- Owner sets key via `metaConfigure(owner.npk, <secp256k1_pubkey>)`

### Serialization Format
`eciesSerialize()` returns: `ephemeralPubHex:ciphertextHex:ivHex:tagHex`
This string is sent directly as the delivery payload — no double hex encoding needed.

---

## 16. Wallet Transfer Format

The wallet module's `transfer_private` and `transfer_public` expect:
- **Recipient**: public keys JSON for private transfers, account ID for public transfers
- **Amount**: 32-character hex string (little-endian 16 bytes), NOT a plain integer

```cpp
// Convert int64 to wallet hex format
static std::string amountToHexLE(int64_t amount) {
    uint8_t bytes[16] = {};
    for (int i = 0; i < 8 && amount > 0; i++) {
        bytes[i] = static_cast<uint8_t>(amount & 0xFF);
        amount >>= 8;
    }
    char hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", bytes[i]);
    hex[32] = '\0';
    return std::string(hex);
}
// 10 LEZ → "0a000000000000000000000000000000"
```

### Wallet-FFI v0.1.0 Limitations
- No `queryProgram` or `callProgram` methods — program interactions are a newer LEZ feature (v0.2.0-rc3)
- `transfer_private` needs recipient's public keys as JSON, not a hex address
- `open()` can return error 99 if wallet storage is locked from a previous process

---

## 17. Spending FSM Error Checking

The wallet module returns non-null QVariant values for errors (e.g., error message strings). Checking `!result.isNull()` alone produces false positives.

```cpp
// WRONG — wallet error string is non-null
bool ok = !result.isNull();

// CORRECT — check content for error indicators
QString resultStr = result.toString();
bool ok = !result.isNull()
    && !resultStr.isEmpty()
    && !resultStr.contains("fail", Qt::CaseInsensitive)
    && !resultStr.contains("error", Qt::CaseInsensitive);
```

---

## 18. Full Test Results (2026-05-21)

### Phase 0: Infrastructure
| Test | Result |
|------|--------|
| Module build (nix) | PASS |
| Module load (6 modules) | PASS |
| Unit tests | 44/44 PASS (72ms) |

### Phase 1: Identity + Wallet
| Test | Result |
|------|--------|
| `pilot.initialize()` | PASS — wallet + identity + ECIES keypair + auto-init deps |
| Identity persistence | PASS — loads on re-run, same NPK |
| `walletBalance()` | PASS — returns balance JSON |
| `walletHistory()` | PASS — returns transactions from SQLite |
| Wallet amount format | PASS — 32-char LE hex |

### Phase 2: Owner Channel (via delivery_module + ECIES)
| Test | Result |
|------|--------|
| `establishOwnerChannel()` | PASS — ECIES-encrypted greeting propagated to 6 Logos relay peers |
| `sendToOwner()` | PASS — ECIES-encrypted message propagated to 6 peers |
| `getOwnerChannelId()` | PASS — Waku content topic returned |
| Incoming message listener | WIRED — event handler decrypts + routes to processOwnerMessage |

### Phase 3: Spending FSM
| Test | Result |
|------|--------|
| `createSpendRequest()` | PASS |
| `setSpendingLimits()` | PASS |
| Over-limit hold flow | PASS — CREATED → HELD → NOTIFIED |
| `approveSpend()` | PASS — state transitions correct |
| `rejectSpend()` | PASS — REJECTED, cleared from pending |
| Under-limit auto-execute | PASS — reports "failed" for invalid recipient |
| Error checking | PASS — detects wallet error strings |

### Phase 4: Storage + Messaging
| Test | Result |
|------|--------|
| `storageUpload()` | PASS — AES-256-GCM encrypt → CID returned |
| `storageList()` | PASS — files with CID + timestamp |
| `storageDownload()` | PARTIAL — reaches storage, local node has no peers |
| `storageShare()` | PASS — ECIES-encrypted CID+key propagated to 6 peers |
| `messagingSend()` | PASS — ECIES encrypted, propagated to 6 peers |
| `messagingJoin()` | PASS — subscribed to group topic |
| `messagingCreateGroup()` | PASS — group created, invite propagated |

### Phase 5: A2A + Blockchain
| Test | Result |
|------|--------|
| `agentCard()` | PASS — full A2A Agent Card with 9 skills published |
| `agentDiscover()` | PASS — subscribed to discovery topic |
| `agentTask()` | PASS — JSON-RPC sent encrypted, propagated to peers |
| `agentSubscribe()` | PASS — task topic subscribed + notification sent |
| `agentCancel()` | PASS — cancel sent + topics unsubscribed |
| `programDeploy` | PASS — approval flow with real binary path |
| `programCall/Query` | TESTED — wallet-ffi v0.1 does not support (version limitation) |

### Meta
| Test | Result |
|------|--------|
| `metaStatus()` | PASS — full JSON with owner_channel set |
| `metaSkills()` | PASS — 21 skills across 6 categories |
| `metaConfigure()` | PASS — owner.npk, LLM, spending limits |
| `processOwnerMessage()` | PASS — slash commands + freetext |
| `dispatchSkill()` | PASS — routes to skill registry |

**Final: 41 PASS / 1 WIRED / 1 PARTIAL / 2 VERSION-LIMITED**

---

## 19. Bugs Fixed Summary

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| `logosAPI_` always NULL | Generator doesn't produce `onInit` when absent from header | Inject `onInit(LogosAPI*)` override via sed |
| `capability_module` call fails | Stub had no `.so` plugin | Install real capability_module from Nix store |
| `Failed to open pilot database` | `sqlite3_open` doesn't create parent dirs | `mkdir()` before open |
| `method not found: "getBalance"` | Wallet uses snake_case | `get_balance` |
| `method not found: "transferPrivate"` | Wallet uses snake_case | `transfer_private` |
| Wallet config deserialization fail | Missing `initial_accounts` field | Added `"initial_accounts": []` |
| Owner channel — chat module broken | callMethod dispatch empty | Switched to delivery_module |
| Storage NAT timeout (20s) | UPnP not available in WSL | `{"nat":"none"}` config |
| Storage upload returns empty | LogosResult not a plain string | `result.value<LogosResult>().value` |
| Storage download wrong method | `downloadFile` doesn't exist | Changed to `downloadChunks` |
| Wallet amount format | Plain int rejected | 32-char LE hex string |
| Spending FSM false success | Wallet error string treated as success | Check result for "fail"/"error" |
| Owner channel plaintext | Messages not encrypted | ECIES encryption with owner public key |
| Owner channel one-way | No receive path | Event listener + decrypt + processOwnerMessage |
| Discovery topic 5 segments | Waku requires 4 segments | `/pilot/1/discovery-X/proto` |
| Waku subscribe fails | Missing clusterId/shards | Added `clusterId:2, shards:[0..7]` |
| Waku peer port wrong | Container port 60000 vs host 30303 | Fixed to host-mapped port |

---

## Integration Findings — 2026-05-25

End-to-end deployment testing. All findings below were verified against live daemon with sequencer running.

### Qt Remote Objects Connection Timing

**Critical finding:** After `load-module`, the Qt Remote Objects replica needs 1-5 seconds to establish the connection. Calling `invokeRemoteMethod` before `isConnected()` returns true causes **SIGSEGV** in `libQt6RemoteObjects.so` (null deref at offset 0x60).

```
dmesg: .logoscore-wrap[28190]: segfault at 60 ip 000072ea8536ed19 sp 00007ffdf07e2990
  error 4 in libQt6RemoteObjects.so.6.9.2[72ea852e6000+ec000]
```

**Resolution:** Poll `client->isConnected()` in 250ms intervals (max 5s) before any `invokeRemoteMethod` call. Applied to `initDependencyModules()` and `initWallet()`.

### Daemon Process Lifecycle

| Issue | Root cause | Resolution |
|-------|-----------|-----------|
| Storage module crash on restart | Stale `logos_host_qt` holding LevelDB lock at `~/.cache/storage/dht/providers/LOCK` | Kill all `logos_host_qt` processes + remove lock before daemon start |
| Wrong PID from `$!` with setsid | `setsid cmd &` — `$!` captures setsid wrapper PID | Read PID from `daemon/state.json` instead |
| Daemon dies on parent exit | Non-interactive bash kills process group on exit | Launch with `setsid` to create new session |
| Initialize timeout | 10s insufficient for wallet → sequencer round-trip | Increased to 30s; wallet connection alone takes 1-5s |

### LLM Integration Findings

| Issue | Root cause | Resolution |
|-------|-----------|-----------|
| API key not reaching module | `putEnv()` in CLI; module runs in separate `logos_host_qt` process | Store via `metaConfigure("llm.api_key", key)` → `setenv()` inside module process |
| LLM error silently swallowed | Error JSON `{"error":"..."}` passes `response.empty()` check | Check for `"error"` substring in response |
| Google Gemini "server replied: " | Google Cloud API keys incompatible with Generative Language API endpoint | Use keys from aistudio.google.com |
| Config lost on daemon restart | `loadIdentity()` didn't restore `llm.api_key` from SQLite | Added `llm.api_key` restoration with correct env var mapping per provider |

### Nim CLI Terminal Findings

| Issue | Root cause | Resolution |
|-------|-----------|-----------|
| Spaces between characters while typing | Linenoise bug #168 — ANSI escape codes in prompt break cursor calculation | Remove ANSI from prompt; use plain ASCII `> ` |
| Backspace erases prompt | Writing prompt via `stdout.write` separately from `readLineFromStdin("")` | Pass prompt string directly to `readLineFromStdin` |
| Multi-byte UTF-8 prompt (`❯`) | Linenoise miscalculates display width of multi-byte chars | Use ASCII-only prompt character |

### Nix Store Discovery

| Depth | What's found | Performance (WSL, 28K entries) |
|-------|-------------|-------------------------------|
| `-maxdepth 1` | Top-level package dirs only | <1s |
| `-maxdepth 2` | Package contents (lib/, share/) but NOT bin/ | ~2s |
| `-maxdepth 3` | Binaries at `<hash>/bin/<name>` | ~4s |
| `-maxdepth 4` | Deep nested files | ~14s |

Correct depth for binary discovery is **3**. Pattern: `find /nix/store -maxdepth 3 -name <binary> -path "*<package-pattern>*" -type f`.

### End-to-End Validation (Single Agent)

Successfully tested full deploy → chat flow:

```
pilot deploy → daemon start → modules load → wallet creates account on sequencer
            → identity persisted → LLM configured → agent card published

pilot chat   → daemon connects → identity restored → LLM responds
            → conversation memory (20 turns) → slash commands formatted
```

44 unit tests passing. All modules load without segfault when `isConnected()` guard is in place.

---

## Integration Findings — 2026-05-26 (Phase 6)

Full phase integration testing and two-agent cross-network testing.

### Single-Agent Phase Testing (28/28)

Ran `test-phases.sh` against a live sequencer. All 28 methods pass across all phases.

| Phase | Methods | Result |
|-------|---------|--------|
| Phase 1: Identity + Wallet | initialize, getAgentNpk, getAccountId, walletBalance, walletHistory | 5/5 |
| Phase 2: Owner Channel | establishOwnerChannel, getOwnerChannelId, sendToOwner | 3/3 |
| Phase 3: Spending FSM | setSpendingLimits, createSpendRequest, getPendingSpends, approveSpend, walletSend | 5/5 |
| Phase 4: Storage | storageUpload, storageList, storageDownload, storageShare | 4/4 |
| Phase 4: Messaging | messagingSend, messagingJoin, messagingCreateGroup | 3/3 |
| Phase 5: A2A Transport | agentCard, agentDiscover, agentTask, agentSubscribe, agentCancel | 5/5 |
| Meta | metaSkills, metaStatus, metaConfigure | 3/3 |

### Bugs Found During Phase Testing

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| `storageDownload` "Chunk size cannot be zero or negative" | Wrong argument order: `downloadChunks(cid, true, 0, path)` — third arg is chunkSize, not offset | Changed to `downloadChunks(cid, false, 65536, path)` |
| `storageShare` crashes module with `std::stoi` | `eciesEncrypt` received full JSON NPK instead of hex viewing key | Extract `viewing_public_key` from JSON; validate hex in `hexToBytes` |
| `agentDiscover` empty/timeout | Called non-existent `waku_module`; delivery_module wraps Waku | Route `storeQuery` through `delivery_module`; add SQLite agent cache |
| `agentTask/Subscribe/Cancel` crash daemon | No `isConnected()` check, no RPC timeout; blocking call freezes module | Added `isConnected()` + `Timeout(15000)` on all A2A delivery calls |
| `initialize` returns false (null = 0 bug) | `wallet->open()` returns null on timeout; `QVariant::toInt()` on null = 0, treated as success | Check `!openResult.isNull()` before trusting `toInt()` |
| `hexToBytes` crash on non-hex input | `std::stoi` throws on `{`, `"`, etc. from JSON strings | Validate all chars are hex before parsing; return empty on invalid |
| `eciesEncrypt` crash on invalid key | Empty bytes from bad hex passed to OpenSSL | Throw `std::invalid_argument`; callers catch and return error JSON |
| `processOwnerMessage` silent LLM failure | LLM returned `{"error":"..."}` which passed `response.empty()` check | Check for `"error"` in response; show actual error message |
| All ECIES callers crash on JSON keys | NPK stored as raw wallet JSON, not extracted hex key | Added `agentViewingKey_` field; extract `viewing_public_key` from JSON everywhere |

### NPK Key Format

The wallet module's `get_private_account_keys` returns JSON:
```json
{
  "nullifier_public_key": "72246aacf616...",   // on-chain identity hash
  "viewing_public_key": "0230d6a870e141..."    // secp256k1 compressed pubkey for encryption
}
```

`nullifier_public_key` = identity (used for on-chain lookups)
`viewing_public_key` = encryption key (used for ECIES encrypt/decrypt)

All ECIES operations must use `viewing_public_key`. The full JSON must never be passed directly to `eciesEncrypt`.

### Lazy Dependency Initialization

`initDependencyModules()` configures storage_module and delivery_module (createNode + start). Originally called during `initialize()`, this blocked for 2+ minutes when delivery did NAT detection.

**Solution:** Removed from `initialize()`. Added `depsInitialized_` flag. Called lazily on first use by: `storageUpload`, `storageDownload`, `storageShare`, `messagingSend`, `establishOwnerChannel`.

Benefits:
- `initialize()` returns in <5 seconds (wallet only)
- Delivery module initializes on first messaging/A2A call
- `depsInitialized_` prevents re-initialization

### Storage Module API Corrections

The storage module uses a three-phase upload and async download:

**Upload (synchronous):**
```
uploadInit(label) → sessionId
uploadChunk(sessionId, QByteArray) → void
uploadFinalize(sessionId) → cid
```

**Download (asynchronous):**
```
downloadChunks(cid, local=false, chunkSize=65536, filepath="") → sessionId
// Data delivered via storageDownloadProgress / storageDownloadDone events
```

`downloadChunks` returns immediately with a session ID. The actual data arrives via events. Our synchronous handler checks if the file was written; if empty, returns `{"status":"downloading"}`.

### Delivery Module Configuration

The delivery module wraps Waku (liblogosdelivery). Key `createNode` config fields:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `preset` | string | `""` | `"logos.dev"` auto-configures cluster 2 + bootstrap nodes |
| `mode` | string | `"noMode"` | `"Core"` (full relay) or `"Edge"` (lightweight client) |
| `tcpPort` | uint16 | `60000` | P2P TCP listen port |
| `clusterId` | uint16 | `0` | Network cluster ID |
| `logLevel` | string | `"INFO"` | Log verbosity |

**NAT detection:** Core mode runs UPnP (8s) + NAT-PMP (4s) detection. In WSL/Docker where there's no real NAT gateway, this wastes 12+ seconds. Use `"nat": "extip:127.0.0.1"` to skip detection (note: `"none"` is NOT valid for delivery, unlike storage).

**Port conflicts:** Two delivery modules on the same machine both bind TCP 60000. Solutions:
- Different `tcpPort` per agent via `PILOT_TCP_PORT` env var
- Docker container with isolated network namespace
- Edge mode for second agent (no relay port binding)

### Two-Agent Integration Testing (14/14)

Ran `test-two-agents-docker.sh` with Agent A on host and Agent B in Docker container.

**Architecture:**
```
┌─────────────────────────────┐     ┌─────────────────────────────┐
│     WSL Host                │     │     Docker Container        │
│                             │     │                             │
│  Agent A (logoscore daemon) │◄───►│  Agent B (logoscore daemon) │
│  - All modules loaded       │     │  - No storage_module        │
│  - TCP 60000 (relay)        │     │  - Own TCP 60000 (isolated) │
│  - Sequencer on :8080       │     │  - Reaches host via gateway │
│                             │     │  - /nix/store mounted :ro   │
└─────────────────────────────┘     └─────────────────────────────┘
         │                                      │
         └──────────┬───────────────────────────┘
                    │
         ┌──────────▼──────────┐
         │   Waku Network      │
         │  (pilot-nwaku:30303)│
         │  + Logos Dev Peers  │
         └─────────────────────┘
```

**Docker setup (no installation required):**
- Mount `/nix/store` read-only — all Nix binaries available in container
- `ubuntu:22.04` base image provides compatible glibc
- `--add-host=host.docker.internal:host-gateway` for sequencer access
- Separate modules directory without storage_module (LevelDB lock conflict)

**Results:**

| Test | Result | Notes |
|------|--------|-------|
| [A] initialize | PASS | Creates unique wallet + identity on sequencer |
| [B] initialize | PASS | Different wallet name (hash of dataDir) → unique identity |
| [A] getAgentNpk | PASS | e97a64fc... |
| [B] getAgentNpk | PASS | eb2860c0... (different from A) |
| [A] publishes Agent Card | PASS | Published to /pilot/1/discovery/proto |
| [B] discovers agents | PASS | Found A's card via Waku store query (10s propagation wait) |
| [A→B] messagingSend | PASS | Encrypted with B's viewing_public_key |
| [B→A] messagingSend | PASS | Encrypted with A's viewing_public_key (Docker delivery has own port) |
| [A] uploads file | PASS | Encrypted + stored via storage_module |
| [A→B] shares file key | PASS | File key encrypted with B's viewing key, sent via delivery |
| [B→A] sends task | PASS | JSON-RPC task request to A's inbox topic |
| [B] subscribes to task | PASS | Subscribed to task status topic |
| [B] cancels task | PASS | Cancel request sent + unsubscribed |
| [A→B] walletSend | PASS | Token transfer via sequencer |

### Platform Limitations Documented

| Limitation | Impact | Workaround |
|-----------|--------|-----------|
| Storage module global LevelDB cache (`~/.cache/storage/`) | Two storage_modules can't coexist on same machine | Separate modules dir for Agent B without storage_module |
| Delivery module TCP port 60000 hardcoded default | Two Core-mode delivery modules conflict | Docker for network isolation, or Edge mode + tcpPort override |
| NAT detection (UPnP/NAT-PMP) in WSL | 12+ second delay on every delivery startup | `extip:127.0.0.1` NAT config skips detection |
| `downloadChunks` is async | Synchronous callers can't get file data immediately | Return "downloading" status; file available via event |
| Qt Remote Objects `Timeout` doesn't cancel in-flight FFI | Synchronous C calls run past the timeout | Set timeouts on all RPC calls; don't block initialize on slow modules |

### Environment Variables for Multi-Agent Configuration

| Variable | Default | Description |
|----------|---------|-------------|
| `PILOT_TCP_PORT` | 60000 | Waku relay TCP port |
| `PILOT_WAKU_MODE` | Core | `Core` (full relay) or `Edge` (lightweight) |
| `PILOT_NAT` | (auto) | NAT config: `extip:127.0.0.1` to skip detection |
| `PILOT_WAKU_ADDR` | /ip4/127.0.0.1/tcp/30303 | Static Waku peer address |
| `PILOT_SEQUENCER_ADDR` | http://127.0.0.1:8080 | LEZ sequencer endpoint |
| `PILOT_DATA_DIR` | /tmp/pilot-data | Agent data directory |

### Final Test Matrix

| Suite | Tests | Pass | Fail |
|-------|-------|------|------|
| Unit tests (`nix build .#unit-tests`) | 44 | 44 | 0 |
| Single-agent (`test-phases.sh`) | 28 | 28 | 0 |
| Two-agent Docker (`test-two-agents-docker.sh`) | 14 | 14 | 0 |
| **Total** | **86** | **86** | **0** |
