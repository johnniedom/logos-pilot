# Owner Channel Architecture

## The Problem

The LP-0008 spec requires an E2E encrypted owner channel over Logos Messaging. The original plan was to use `chat_module` which handles Waku transport and encryption as a single package.

### How Logos Module IPC Works

Each Logos Core module runs in its own process. They communicate through Qt Remote Objects — a system where one module **publishes** an object (provider) and other modules **connect** to a copy of that object (replica).

The key concepts:

- **Provider** — the module that owns the real object. It publishes the object so others can connect. The provider handles incoming method calls and emits events (signals) to all connected replicas.
- **Replica** — a remote copy of the provider's object. Other modules get replicas to interact with the provider. Replicas can call methods on the provider and receive events from it.
- **callMethod** — when a replica calls a method, the Qt Remote Objects framework serializes the call and sends it to the provider, which dispatches it in a `callMethod()` function.
- **Events (signals)** — flow one direction only: from provider to replica. The provider emits a signal, all connected replicas receive it. Replicas cannot emit events back to the provider.

Visually:

```
Provider (chat_module)          Replica (pilot)
┌──────────────┐               ┌──────────────┐
│              │  ◄─ methods ──│              │
│  callMethod  │               │  .invoke()   │
│  dispatch    │               │              │
│              │               │              │
│  emit signal ──  events ──►  │  .onEvent()  │
│              │               │              │
└──────────────┘               └──────────────┘
```

### What Went Wrong With chat_module

When pilot gets a replica of `chat_module` and calls methods on it (e.g. `chatInit`, `chatStart`, `chatCreateIntroBundle`), the calls arrive at the IPC layer but chat's provider drops them silently.

Two problems discovered through live testing:

**1. Empty callMethod dispatch.** The `ChatModuleProviderObject` has a `callMethod()` function but it contains no dispatch logic — no `if (method == "chatInit")` or equivalent. Method calls from other modules arrive and are discarded.

**2. One-directional events.** Events flow only from provider (chat) to replica (pilot). When pilot emits an event on its replica, it does not reach chat's provider. The Qt Remote Objects event system is designed for the provider to broadcast to clients, not receive from them.

Evidence from live testing:

```
# Chat module logs — only during startup, silence after
ChatModuleImpl: Initializing...
ChatModuleImpl: Initialized successfully
LogosProviderBase::init called

# Pilot module logs — events fire, IPC connects, chat never responds
[pilot] Requesting intro bundle from chat_module...
[pilot] Event sent: chatCreateIntroBundle
[pilot] Subscribed to chat event responses
# ... no response from chat_module. Ever.
```

### Root Cause

The chat module was built for the QML UI (Basecamp desktop app), not for module-to-module communication. In the UI setup:

```
chat_module (provider)  ──events──►  QML UI (replica)
                        ◄─methods──
```

The QML UI calls methods on chat (send message, create conversation) and chat pushes events to the UI (message received, conversation created). This works because the QML `ChatModule` client class has proper event listeners wired to UI elements.

But in the module-to-module setup, pilot is another module trying to act like the UI:

```
chat_module (provider)  ──events──►  pilot (replica)
                        ◄─methods──  (callMethod is empty,
                                      calls are dropped)
```

The events from chat to pilot work (pilot can receive them). But pilot cannot trigger any action in chat because the method dispatch is empty.

## The Solution: delivery_module Bypass

Instead of going through chat_module, the owner channel uses `delivery_module` for Waku transport and handles encryption directly using pilot's existing ECIES implementation.

### Why delivery_module Works

The `delivery_module` wraps the Waku protocol directly. Unlike chat_module, its `callMethod()` dispatch is fully implemented — it handles `subscribe`, `send`, `unsubscribe`, and other operations from any module that calls them. This was confirmed by live testing: pilot called `delivery_module.subscribe()` and received a success response.

### Architecture

```
┌──────────────────────────────────────────────────────┐
│                     logoscore                         │
│                                                      │
│  ┌──────────────┐         ┌───────────────────────┐  │
│  │delivery_module│◄──IPC──│     pilot_module       │  │
│  │              │         │                       │  │
│  │ .subscribe() │         │  pilot_owner.cpp      │  │
│  │ .send()      │         │  ┌─────────────────┐  │  │
│  │ .onMessage() │         │  │ ECIES encrypt   │  │  │
│  └──────┬───────┘         │  │ ECIES decrypt   │  │  │
│         │                 │  │ processMessage  │  │  │
│         │                 │  └─────────────────┘  │  │
│         │                 └───────────────────────┘  │
│    ┌────┴────┐                                       │
│    │  Waku   │                                       │
│    │  Node   │                                       │
│    └────┬────┘                                       │
└─────────┼────────────────────────────────────────────┘
          │
     ┌────┴─────────────────────────────────┐
     │        Logos Waku Relay Network       │
     │  Amsterdam · US-Central · Hong Kong  │
     │          6 relay peers               │
     └──────────────────────────────────────┘
```

### Key Variables

- `ownerNpk_` (std::string) — Owner's public key (NPK). Set via `metaConfigure("owner.npk", key)`. Persisted in SQLite `config` table. Used as the ECIES encryption target for outgoing messages.
- `ownerTopic_` (std::string) — The Waku content topic for this owner channel. Format: `/pilot/1/owner-{accountId}/proto`. Both agent and owner subscribe to this topic.
- `accountId_` (std::string) — The agent's LEZ wallet account ID. Created during `initialize()` via `create_account_private()`. Used in the topic string because it is shorter and more stable than the full NPK.
- `ownerChannelId_` (std::string) — Stores the active topic after `establishOwnerChannel()` succeeds. Returned by `getOwnerChannelId()`.

### Setup Flow

```
1. Owner configures their public key:
   pilot.metaConfigure("owner.npk", "<owner_public_key_hex>")
   
   → ownerNpk_ = key
   → Persisted to SQLite: INSERT INTO config (key, value) VALUES ("owner.npk", key)
   → Survives restarts via loadConfig()

2. Owner channel established:
   pilot.establishOwnerChannel()
   
   → Check: ownerNpk_ must be set (returns false if not)
   → Build topic: "/pilot/1/owner-" + accountId_ + "/proto"
   → Call delivery_module.subscribe(ownerTopic_)
   → Waku node subscribes to topic across relay network
   → Encrypt greeting with eciesEncrypt(ownerNpk_, greeting)
   → Broadcast encrypted greeting via delivery_module.send()
   → Store topic in ownerChannelId_
```

### Sending Messages (Agent to Owner)

```
pilot.sendToOwner("Your balance is 150 LEZ")

Step 1: Convert message to bytes
   plainBytes = std::vector<uint8_t>(message.begin(), message.end())

Step 2: ECIES encrypt with owner's public key
   ECIESCiphertext encrypted = eciesEncrypt(ownerNpk_, plainBytes)
   
   Internally:
   a. Generate ephemeral secp256k1 keypair (used once, discarded after)
   b. ECDH: ephemeral_private_key x ownerNpk_ = shared_secret
   c. SHA256(shared_secret) = aes_key (32 bytes)
   d. Generate random 12-byte IV
   e. AES-256-GCM encrypt: plaintext + aes_key + IV = ciphertext + auth_tag
   f. Package: ECIESCiphertext {
        ephemeralPub,  // so recipient can derive the same shared secret
        iv,            // 12 bytes
        ciphertext,    // encrypted message
        tag            // 16-byte authentication tag (tamper proof)
      }

Step 3: Serialize ECIESCiphertext to bytes
   serialized = ephemeralPub || iv || ciphertext || tag

Step 4: Hex-encode (Waku transport expects hex strings)
   hexPayload = toHex(serialized)

Step 5: Broadcast via delivery_module
   delivery_module.send(ownerTopic_, hexPayload)
   → Waku node propagates to all subscribed peers (6 relay nodes)
```

Only the owner can decrypt this message. They perform the reverse:
- Extract ephemeral public key from the message
- ECDH: owner_private_key x ephemeral_pub = same shared_secret
- SHA256(shared_secret) = same aes_key
- AES-256-GCM decrypt using aes_key + IV + auth_tag = plaintext

### Receiving Messages (Owner to Agent)

```
Owner sends encrypted message to the same Waku topic

Step 1: delivery_module receives message on ownerTopic_
   → Emits "message received" event with payload and topic

Step 2: Pilot's event handler catches the event
   → Filter: is topic == ownerTopic_? If not, ignore.

Step 3: Hex-decode payload back to bytes

Step 4: Deserialize into ECIESCiphertext struct
   → Extract ephemeralPub, iv, ciphertext, tag

Step 5: ECIES decrypt with agent's private key
   plaintext = eciesDecrypt(agentPrivateKey, encrypted)
   
   Internally:
   a. ECDH: agent_private_key x ephemeralPub = shared_secret
   b. SHA256(shared_secret) = aes_key
   c. AES-256-GCM decrypt: ciphertext + aes_key + IV + tag = plaintext
   d. If tag verification fails, message was tampered with — discard

Step 6: Route to processOwnerMessage(plaintext)
   → Starts with "/"? Slash command, route to dispatchSkill()
   → Freetext + LLM configured? Send to llmProvider_->chat(), parse response, dispatchSkill()
   → No LLM? Return help text listing available /commands

Step 7: Send response back
   response = skill result or LLM-formatted answer
   sendToOwner(response) → ECIES encrypt → delivery_module → Waku → owner
```

### The Complete Loop

```
Owner                          Waku Network                    Agent
  |                                |                             |
  |  encrypt(agentNpk, "balance")  |                             |
  |------------------------------->|                             |
  |                                |---------------------------->|
  |                                |  delivery_module event      |
  |                                |                    decrypt()|
  |                                |            processMessage() |
  |                                |              walletBalance()|
  |                                |                             |
  |                                |  encrypt(ownerNpk, "150 LEZ")
  |                                |<----------------------------|
  |<-------------------------------|                             |
  |  decrypt() = "150 LEZ"         |                             |
```

### ECIES Encryption Details

ECIES (Elliptic Curve Integrated Encryption Scheme) combines three primitives:

```
1. Key Agreement:  ECDH on secp256k1
   - Sender generates ephemeral keypair (e_priv, e_pub)
   - Shared secret = ECDH(e_priv, recipient_pub)
   - Recipient recovers: ECDH(recipient_priv, e_pub) = same shared secret

2. Key Derivation:  SHA-256
   - aes_key = SHA256(shared_secret)
   - 32 bytes = 256-bit AES key

3. Symmetric Encryption:  AES-256-GCM
   - Authenticated encryption (encrypt + integrity check)
   - 12-byte random IV per message
   - 16-byte authentication tag (proves no tampering)
   - If tag verification fails on decrypt, message is rejected
```

Implementation: `pilot_crypto.cpp` using OpenSSL 3.x EVP API (`OSSL_PARAM_BLD`, not deprecated `EC_KEY`).

### Why This Approach Is Better

1. **No black-box dependency.** Pilot owns its encryption. The code is in `pilot_crypto.cpp`, tested with 8 unit tests covering round-trips, wrong-key detection, and large data.

2. **Framework-bug-proof.** If the Logos team fixes chat_module's callMethod dispatch tomorrow, nothing breaks. If they don't fix it, pilot still works.

3. **Reusable pattern.** Any Logos module that needs encrypted module-to-module messaging can use the same approach: delivery_module for transport + ECIES for encryption.

4. **Same transport for everything.** The owner channel, A2A discovery, A2A tasks, and messaging skills all use delivery_module. One transport layer, one set of bugs to fix, one integration to maintain.

### Comparison: chat_module vs delivery_module Bypass

- **Transport:** chat_module uses Waku via chat. delivery_module bypass uses Waku directly.
- **Encryption:** chat_module has built-in E2E. Bypass uses pilot's own ECIES (pilot_crypto.cpp).
- **Module-to-module calls:** chat_module is broken (empty callMethod). delivery_module works (tested and confirmed).
- **Dependency control:** chat_module is external (Logos team). Bypass is internal (our code).
- **Test coverage:** chat_module has none (cannot trigger it). Bypass has 8 crypto unit tests + live integration.
- **Network verification:** chat_module never reached Waku. Bypass confirmed: 6 peers, 3 data centers, message hash verified.

### Files Changed

- `pilot_owner.cpp` — Rewritten: chat_module calls replaced with delivery_module + ECIES
- `pilot_impl.cpp` — Updated: event listener for incoming delivery_module messages
- `pilot_crypto.cpp` — Unchanged: ECIES implementation already existed
- `pilot_crypto.h` — Unchanged: ECIESCiphertext struct already defined
- `metadata.json` — chat_module can be removed from dependencies (delivery_module already listed)
