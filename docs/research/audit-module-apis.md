# LP-0008 Module API Audit

> Conducted 2026-05-14. Four modules audited in parallel from GitHub source.
> Purpose: validate assumptions before scaffolding Pilot repo.

---

## Verdict Summary

| Module | LP-0008 Can Use? | Blockers |
|--------|-----------------|----------|
| lez_wallet_module | YES | None — multi-account creation fully supported |
| delivery_module | YES (with caveats) | No encryption, no persistence, no ordering |
| storage_module | YES (with caveats) | No encryption, no identity-based sharing |
| agent/identity | BUILD FROM SCRATCH | Nothing exists; use `key_protocol::KeyChain` |

---

## 1. lez_wallet_module

**Source:** `logos-blockchain/logos-execution-zone-wallet-ui` + `logos-blockchain/logos-execution-zone/wallet-ffi/`

### Can it create a second account?

YES. The wallet is designed for multiple accounts. No hardcoded limit.

- `createAccountPublic()` and `createAccountPrivate()` can each be called repeatedly
- `wallet_ffi_list_accounts()` returns a dynamic `FfiAccountList` with growable entries
- CLI confirms: `wallet account new public` and `wallet account new private` are repeatable

**For LP-0008:** Call `createAccountPrivate()` on the existing wallet instance. No need for a second *wallet* — just a second *account*.

### Qt Remote Objects Interface (13 slots)

```
READONLY properties:
  isWalletOpen        : bool
  configPath          : QString
  storagePath         : QString
  lastSyncedBlock     : int
  currentBlockHeight  : int
  sequencerAddr       : QString

Slots:
  createAccountPublic()           → QString (account ID hex)
  createAccountPrivate()          → QString (account ID hex)
  refreshAccounts()               → void
  getBalance(accountIdHex, isPublic) → QString
  refreshBalances()               → void
  getPublicAccountKey(accountIdHex)  → QString
  getPrivateAccountKeys(accountIdHex) → QString
  syncToBlock(blockId)            → bool
  transferPublic(from, to, amount)   → QString
  transferPrivate(from, to, amount)  → QString
  transferPrivateOwned(from, to, amount) → QString
  createNew(configPath, storagePath, password) → bool
  copyToClipboard(text)           → void
```

### Public vs Private Account Creation

Fundamentally different:
- **Public:** Address visible on-chain, balance publicly queryable
- **Private (shielded):** Balance and activity private, identifier randomly assigned
- **Private keys only:** `create_private_accounts_key()` generates key material without registering an account

Transfer types:
- `transferPublic` — public-to-public
- `transferPrivate` — private-to-private (fully shielded)
- `transferPrivateOwned` — between same-wallet accounts

### LP-0008 Integration Path

1. Call `createAccountPrivate()` via Qt Remote Objects → get shielded account ID
2. Owner funds via `transferPrivateOwned()` from owner's account
3. Agent uses `transferPrivate()` for spending (subject to approval state machine)
4. Check balance via `getBalance(accountIdHex, false)` (false = private)
5. Periodic `syncToBlock()` to keep private state current

**VERDICT: No blockers.** Full multi-account support, all programmatic.

### Key Files
- `.rep` interface: `logos-blockchain/logos-execution-zone-wallet-ui/src/LEZWalletBackend.rep`
- C FFI: `logos-blockchain/logos-execution-zone/wallet-ffi/wallet_ffi.h` (47 functions)
- Module interface: `logos-blockchain/logos-execution-zone-module/src/i_logos_execution_zone_wallet_module.h` (31 virtual methods)

---

## 2. delivery_module

**Source:** `logos-co/logos-delivery-module/`

### Topic Subscribe/Unsubscribe

FULLY SUPPORTED with arbitrary content topics.

```
subscribe("/a2a/1/inbox-{agentId}/proto")    → listens for that topic only
unsubscribe("/a2a/1/inbox-{agentId}/proto")  → stops listening
send("/a2a/1/inbox-{agentId}/proto", payload) → publishes to that topic
```

Topics are arbitrary strings — no registry, no predefined list, no creation API needed. Recommended Waku format: `/{app}/{version}/{topic-name}/{encoding}`.

### Qt Remote Objects Interface (9 methods + 5 events)

```
Methods:
  createNode(configJson)           → LogosResult (init node)
  start()                          → LogosResult
  stop()                           → LogosResult
  send(contentTopic, payload)      → LogosResult (with requestId)
  subscribe(contentTopic)          → LogosResult
  unsubscribe(contentTopic)        → LogosResult
  getAvailableNodeInfoIDs()        → LogosResult
  getNodeInfo(nodeInfoId)          → LogosResult
  getAvailableConfigs()            → LogosResult

Events (via eventResponse signal):
  messageSent        [requestId, messageHash, timestamp]
  messageError       [requestId, messageHash, error, timestamp]
  messagePropagated  [requestId, messageHash, timestamp]
  messageReceived    [messageHash, contentTopic, payload(base64), timestamp(ns)]
  connectionStateChanged [connectionStatus, timestamp]
```

### Encryption

**THE DELIVERY MODULE HAS NO ENCRYPTION.** `send()` takes plaintext, base64-encodes internally.

Two options for LP-0008's encrypted owner channel:
1. **Use `logos-chat-module`** — provides E2E encryption via intro-bundle key exchange. `requestMyBundle()` / `createConversation(bundle, message)` pattern. Auto-establishes on first boot. Ready-made, conversation-oriented (1:1).
2. **DIY encryption on top of delivery** — encrypt payloads before `send()`, decrypt on `messageReceived`. Full control but more work.

**Recommendation:** Use chat module for owner channel (it's exactly that use case). Use DIY ECIES encryption for A2A messages (LP-0008 research doc already designed this).

### Limitations for LP-0008

1. **No message persistence** — fire-and-forget. Need Waku Store (`WakuModuleInterface::storeQuery()`) for replay; exposed on waku_module but NOT on delivery_module
2. **No ordering guarantees** — need application-level sequence numbers
3. **No delivery receipts** — `messagePropagated` means "reached network," not "reached recipient"
4. **Single node per plugin instance** — multiple agents on same host share one delivery node, differentiate by topic subscriptions
5. **Content-topic bloat warning** — Waku spec warns against too many topics degrading Store/Filter performance. Design topic namespace carefully.

### LP-0008 Topic Structure (validated against API)

```
/pilot/1/discovery/proto               → Agent Cards (public broadcast)
/pilot/1/inbox-{npk-hash}/proto        → Per-agent inbox (ECIES encrypted)
/pilot/1/reply-{request-uuid}/proto    → Ephemeral reply channel
/pilot/1/task-{task-id}/proto          → Streaming task updates
/pilot/1/owner-{agent-id}/proto        → Owner channel (via chat module E2E)
```

**VERDICT: Usable, but encryption and persistence are on us.**

### Key Files
- Plugin header: `logos-co/logos-delivery-module/src/delivery_module_interface.h`
- C FFI: `logos-co/logos-delivery-module/tests/stubs/lib/liblogosdelivery.h` (12 functions)
- Chat module: `logos-co/logos-chat-module/src/chat_module_plugin.h`
- Waku module: `logos-co/logos-waku-module/waku_module_interface.h`

---

## 3. storage_module

**Source:** `logos-co/logos-storage-module/` + `logos-co/logos-storage-ui/`

### Does LP-0008 Need It?

YES. The spec requires four storage skills:
- `storage.upload(path, label)` — "encrypts and uploads"
- `storage.download(address, path)` — "retrieves and decrypts"
- `storage.list()` — "lists files with labels and content addresses"
- `storage.share(address, recipient)` — "shares access with another Logos identity"

LP-0017's document-indexing module is orthogonal (upload → broadcast → anchor). It does NOT provide encryption or sharing. LP-0008 calls the storage module directly and builds encryption/sharing on top.

### Qt Remote Objects Interface

```
Properties:
  debugLogs          : QString (READONLY)
  status             : StorageStatus enum (READONLY)
  defaultListenPort  : int (READONLY)
  defaultConfigJson  : QString (READONLY)

StorageStatus enum: Stopped=0, Starting=1, Running=2, Stopping=3, Destroyed=4

Upload/Download Slots:
  uploadFile(url)                        → void (CID via uploadCompleted signal)
  downloadFile(cid, url, totalBytes)     → void
  downloadManifest(cid)                  → void
  downloadManifests()                    → void
  exists(cid)                            → void
  remove(cid)                            → void
  fetch(cid)                             → void

Lifecycle Slots:
  init(configJson)                       → void
  start()                                → void
  stop()                                 → void
  destroy()                              → void

Config Slots:
  saveUserConfig(configJson)             → void
  saveCurrentConfig()                    → void
  loadUserConfig()                       → void
  getUserConfig()                        → QString
  reloadIfChanged(configJson)            → void
  configJson()                           → QString

Network Slots:
  enableUpnpConfig()                     → void
  enableNatExtConfig(tcpPort)            → void
  checkNodeIsUp()                        → void
  refreshSpace()                         → void

Signals:
  uploadStarted(totalBytes)
  uploadChunk(len)
  uploadCompleted(cid)                   ← THIS is how you get the CID back
  downloadStarted(cid, filename, totalBytes)
  downloadChunk(len)
  downloadCompleted(cid)
  manifestsUpdated(manifests: QVariantList)
  spaceUpdated(total, used)
  error(message)
  initCompleted(success, error)
  startCompleted()
  startFailed(error)
  nodeIsUp() / nodeIsntUp(reason)
```

### Plugin-Level API (richer than .rep)

The `StorageModulePlugin` exposes additional programmatic methods beyond the UI's `.rep`:
- **Chunked upload:** `uploadInit(filename, chunkSize)` → `uploadChunk(sessionId, chunk)` → `uploadFinalize(sessionId)` → CID
- **Chunked download:** `downloadChunks(cid, local, chunkSize, filepath)` → streams via events
- **Batch:** `importFiles(path)` → bulk upload with timeouts
- **Node info:** `peerId()`, `spr()`, `debug()`, `version()`, `dataDir()`

### Encryption

**NO ENCRYPTION AT ANY LAYER.**
- No encryption parameters in `.rep`
- No encryption logic in `StorageModulePlugin`
- No encryption functions in `libstorage.h` C API
- No encryption endpoints in Codex REST API

LP-0008 must build:
1. Encrypt file content BEFORE passing to `uploadFile()`
2. Decrypt AFTER `downloadFile()`
3. Manage per-file symmetric keys tied to agent's NPK/SSK
4. For `storage.share()` — re-encrypt or share decryption key with recipient over encrypted Logos Messaging

### Content Addressing

CIDv1 with SHA-256 multihash (per Codex spec). Each upload returns a CID string via `uploadCompleted(cid)` signal. Manifest contains: `treeCid`, `datasetSize`, `blockSize`, optional `filename`, optional `mimetype`.

### Identity-Based Sharing

**DOES NOT EXIST.** Anyone with a CID can download. No access control, no permissions.

LP-0008's `storage.share(address, recipient)` must be built as:
1. Encrypt file with per-file AES-256-GCM key
2. Upload encrypted blob → get CID
3. Encrypt the file key to recipient's NPK via ECIES
4. Send (CID + encrypted key) to recipient over Logos Messaging

### NAT Traversal

Required for file sharing. Three pathways:
1. UPnP/NAT-PMP (`enableUpnpConfig()`)
2. Manual port forward (`enableNatExtConfig(tcpPort)`)
3. Advanced JSON config

Default ports: TCP 8500, UDP 8090. Reachability check: `checkNodeIsUp()` → `nodeIsUp`/`nodeIsntUp`.

**VERDICT: Usable for raw upload/download. Encryption and sharing are entirely on us.**

### Key Files
- `.rep` interface: `logos-co/logos-storage-ui/src/StorageBackend.rep`
- Module plugin: `logos-co/logos-storage-module/src/storage_module_plugin.cpp`
- C API: `logos-storage/logos-storage-nim/library/libstorage.h`

---

## 4. Agent/Identity Infrastructure

### Does Logos Have an Agent Module?

**NO.** Searched all 90+ repos across `logos-co` and `logos-blockchain`. Zero agent modules, zero agent frameworks, zero agent runtimes.

### logos-capability-module

**IRRELEVANT to LP-0008.** It coordinates inter-module permissions ("can module X call method Y on module Z"). Uses `requestModule(fromModuleName, moduleName)` to issue auth tokens. Has nothing to do with agent identity, KeyChain, or A2A capabilities.

### key_protocol — The Identity Foundation

**Located at:** `logos-blockchain/logos-execution-zone/key_protocol/`

This is what LP-0008 MUST use for agent identity:

```rust
pub struct KeyChain {
    pub secret_spending_key: SecretSpendingKey,      // SSK — the catastrophic secret
    pub private_key_holder: PrivateKeyHolder,        // NullifierSecretKey + ViewingSecretKey
    pub nullifier_public_key: NullifierPublicKey,    // NPK — public identity
    pub viewing_public_key: ViewingPublicKey,        // VPK — view/decrypt derivation
}

// Agent identity creation — zero deps, zero user interaction
let agent_keys = KeyChain::new_os_random();
```

Also available:
- `KeyChain::new_mnemonic(passphrase)` — BIP-39 recovery (for user-facing wallets)
- `GroupKeyHolder` — shared/multi-controller accounts (not needed for LP-0008)
- `EphemeralKeyHolder` — one-time DH key exchange for transaction encryption
- `KeyTreePublic`/`KeyTreePrivate` — HD key derivation

### Prior LP-0008 Submission: Beach-Bum's Agora (REJECTED)

PR #34 closed 2026-04-28, zero reviewer comments. Repo: `Beach-Bum/agora-agent`.

**Why it failed (probable):**
- Built identity in Python using `secrets.token_bytes(32)` + OS keychain — completely disconnected from LEZ's `key_protocol`
- Built unnecessary on-chain contracts (identity NFT with stake + reputation + escrow) not required by spec
- Dual C++/QML + Python architecture

**What to steal:** Skill categorization (21 skills, 5 categories) and A2A topic structure. NOT the identity layer.

### Related Modules

- `logos-chat-module` — Has E2E encryption via intro-bundle exchange. Useful for owner channel.
- `logos-waku-module` — Waku networking wrapper. Lower-level than delivery module. Has `storeQuery()` and `filterSubscribe()`.
- `logos-module-builder` — Standard CMake module for building/packaging Logos Qt plugins.

**VERDICT: Build from scratch, on top of `key_protocol::KeyChain`. No existing agent infrastructure to extend.**

---

## 5. accounts_module (supplementary — found by local agent)

**Source:** `logos-co/logos-accounts-module/`

**NOTE:** This is an Ethereum-style account module, separate from the LEZ wallet in `logos-blockchain`. Discovered by Johnnie's local Logos agent during a parallel audit. Not the primary path for LP-0008 (which needs LEZ shielded accounts), but documents what's available.

### API

```
Account Creation:
  keystoreNewAccount(passphrase)           → create new account
  extKeystoreNewAccount(passphrase)        → create extended key account
  createRandomMnemonic(length)             → generate seed phrase

Key Management:
  createExtKeyFromMnemonic(phrase, pass)   → derive keys from mnemonic
  deriveExtKey(extKeyStr, pathStr)         → HD derivation (BIP-32/44)
  extKeyToECDSA(extKeyStr)                → get private key
  ecdsaToPublicKey(privateKey)             → get public key
  publicKeyToAddress(publicKey)            → get address

Signing:
  keystoreSignHash(address, hashHex)       → sign arbitrary data
  keystoreSignTx(address, txJSON, chainID) → sign transactions
  keystoreUnlock / keystoreLock / keystoreTimedUnlock → session management
```

**VERDICT: Not needed for LP-0008's primary path** (LEZ wallet handles shielded accounts). But useful if the agent ever needs to interact with Ethereum-side infrastructure or needs ECDSA signing for non-LEZ purposes.

---

## Important: Two GitHub Orgs

The Logos ecosystem spans TWO GitHub organizations:
- **`logos-co`** — 130+ repos. Delivery, storage, chat, waku, accounts modules. Most Logos Core plugins.
- **`logos-blockchain`** — Separate org. LEZ execution zone, wallet FFI, wallet UI, key_protocol.

Searching only `logos-co` will miss the entire LEZ wallet stack. Both orgs must be checked for any complete audit.

---

## Critical Engineering Decisions Informed by This Audit

### D11. How Pilot calls other modules

Two options discovered:
- **Qt Remote Objects (inter-module RPC)** — standard module communication path. Use `LogosAPIClient` / `ModuleProxy::callRemoteMethod(authToken, method, args)`. Clean separation.
- **Direct FFI linking** — link against `wallet_ffi`, `libstorage`, `liblogosdelivery`. Faster but tighter coupling.

**Recommendation:** Qt Remote Objects for wallet + storage + delivery (spec says "module loaded alongside other modules"). FFI only if Remote Objects latency is unacceptable for specific operations.

### D12. Owner channel encryption strategy

- **Use `logos-chat-module`** for the owner channel. Auto-establish on first boot: agent calls `requestMyBundle()`, receives bundle via `bundleReady()` signal, calls `createConversation(bundle, "Pilot connected.")`. Owner opens Basecamp Chat and the conversation is already there. Zero manual steps. Fallback: show bundle in UI with Copy button; owner pastes in Chat App's "New Conversation" screen (not the message box). Messages are text-only via `sendMessage(conversationId, content)`.
- **Use ECIES on top of raw delivery** for A2A inter-agent messages (as designed in research doc).

### D13. Storage encryption architecture

The storage module is a dumb byte pipe. LP-0008 needs:
1. AES-256-GCM per-file encryption before upload
2. Key derivation from agent's SSK (or random per-file keys stored in SQLite)
3. `storage.share()` = encrypt the file key to recipient's NPK + send over Messaging

### D14. Message persistence strategy

Delivery module is fire-and-forget. For LP-0008's crash recovery (research doc problem 4):
- Waku Store protocol is on `waku_module` (NOT delivery_module)
- Either depend on `waku_module` directly for `storeQuery()`
- Or accept message loss during downtime and rely on retry/re-notification

### D15. Waku module dependency (NEW — discovered in audit)

The research doc designed around Waku Store for race-window mitigation and crash recovery. But Store is on `waku_module`, not `delivery_module`. Options:
1. Depend on both `delivery_module` AND `waku_module`
2. Use only `delivery_module` and accept its limitations
3. Use only `waku_module` (lower-level, more control, more work)

**Recommendation:** Depend on both. Use delivery for pub/sub, waku for Store queries. This is what the chat module does — it layers on top of both.

---

## Assumptions Validated

- ✅ Agent can create its own shielded account (lez_wallet_module supports multi-account)
- ✅ Arbitrary topic subscribe/unsubscribe works (delivery_module is topic-agnostic)
- ✅ Programmatic upload/download works (storage_module has full API)
- ✅ `KeyChain::new_os_random()` exists for agent identity creation
- ✅ No competing agent module exists (clear field)

## Assumptions INVALIDATED

- ❌ "Delivery module handles encryption" — it does NOT. Build on top.
- ❌ "Storage module handles encryption" — it does NOT. Build on top.
- ❌ "LP-0017 storage work reusable for LP-0008" — orthogonal use cases.
- ❌ "Delivery module has message persistence" — it does NOT. Need waku_module for Store.
- ❌ "logos-capability-module is relevant" — it's for inter-module permissions, not agent capabilities.

## Wrong Assumptions That Would Have Cost Days

1. **Building on `logos-capability-module` for agent capabilities** — would have been a dead end. It's about module permissions, not agent skills.
2. **Expecting delivery to handle encryption** — would have discovered at integration time that owner channel messages are plaintext.
3. **Expecting storage to handle file encryption** — would have discovered at integration time that `storage.upload` sends cleartext to Codex.
4. **Trying to reuse LP-0017's module for LP-0008 storage skills** — different layer entirely.
5. **Not depending on `waku_module`** — would have lost crash recovery capability.
6. **Following Agora's Python identity approach** — would have gotten rejected for the same reason.
