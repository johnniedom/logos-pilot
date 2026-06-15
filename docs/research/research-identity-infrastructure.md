# LP-0008 -- Logos Identity/Agent Infrastructure Research

> Research conducted 2026-05-14. Comprehensive audit of all identity, capability, agent, and key management components in the Logos ecosystem.

---

## Executive Summary

**LP-0008 must build agent identity from scratch.** There is no existing agent identity module, no agent framework, and no reusable identity abstraction in the Logos ecosystem. However, there is excellent low-level key management infrastructure in the LEZ `key_protocol` crate that our agent wallet MUST use (not reinvent). The capability module exists but is purely inter-module permission coordination, not agent-level identity. One prior LP-0008 submission (Beach-Bum/Agora) was rejected -- it built identity entirely in Python with its own keystore, which is likely one reason it failed.

---

## 1. Does Logos Already Have an Agent Identity Module?

**No.** Searched all 90+ repos across `logos-co` and `logos-blockchain` orgs. No repo matches "agent" in name or description. No module in the `logos-modules` meta-repo handles agent identity.

The closest things that exist:
- `logos-accounts-module` -- Ethereum-style account management via go-wallet-sdk (mnemonic, keystore, ECDSA). NOT LEZ-native. Wraps a Go SDK for traditional blockchain accounts. 35+ tests but no integration with LEZ's NSSA/key_protocol system.
- `logos-wallet-module` -- WIP balance/transaction module. Also wraps go-wallet-sdk. Does NOT generate LEZ-native KeyChains.
- `logos-capability-module` -- Permission coordination between modules (see section 2).

**Conclusion: Agent identity is a greenfield build.**

---

## 2. The Capability Module

**Repo:** `logos-co/logos-capability-module`
**What it does:** Coordinates permissions and capabilities between Logos Core modules. It is a Qt plugin built with `logos-module-builder`.
**What it exposes:** A `LogosProviderBase` C++ class. Permission checks between modules.

**What it does NOT do:**
- No agent identity
- No KeyChain/NPK/SSK
- No wallet integration
- No on-chain identity
- No capability declaration for agents

**Verdict:** This is infrastructure-level permission plumbing (e.g., "can module X call method Y on module Z"). It has nothing to do with agent identity or A2A capability cards. LP-0008's agent capability system (Agent Cards, skill registry) must be built separately. The capability module might be relevant for sandboxing skill execution permissions, but that's a stretch goal, not a dependency.

---

## 3. Key Management: How It Works

### 3.1 The `key_protocol` Crate (LEZ-native, Rust)

**Location:** `logos-blockchain/logos-execution-zone/key_protocol/`
**This is the authoritative key management code.** It implements the full LEZ identity stack:

#### KeyChain (the core identity primitive)
```rust
pub struct KeyChain {
    pub secret_spending_key: SecretSpendingKey,
    pub private_key_holder: PrivateKeyHolder,
    pub nullifier_public_key: NullifierPublicKey,
    pub viewing_public_key: ViewingPublicKey,
}
```

Two constructors:
- `KeyChain::new_os_random()` -- generates from OS entropy (for automated/programmatic use)
- `KeyChain::new_mnemonic(passphrase)` -- generates from BIP-39 mnemonic (for user-facing wallets)

#### Key Hierarchy
```
SeedHolder (64 bytes from BIP-39 mnemonic or OS random)
  -> SecretSpendingKey (32 bytes, HMAC-SHA512 derived, 2048 iterations)
    -> PrivateKeyHolder
      -> NullifierSecretKey (for spending authorization)
      -> ViewingSecretKey (for balance decryption)
    -> NullifierPublicKey (NPK -- public identity for nullifiers)
    -> ViewingPublicKey (VPK -- public identity for encrypted notes)
```

#### GroupKeyHolder (for shared accounts)
```rust
pub struct GroupKeyHolder { gms: [u8; 32] }  // Group Master Secret
```
- Derives per-PDA keys: `derive_keys_for_pda(program_id, pda_seed) -> PrivateKeyHolder`
- Derives shared account keys: `derive_keys_for_shared_account(derivation_seed) -> PrivateKeyHolder`
- Secure distribution via seal/unseal (ECDH + AES-256-GCM)
- Domain-separated from personal keys (no collision possible)

#### EphemeralKeyHolder (for one-time key exchange)
- Single-use DH key exchange for shared secret derivation
- Used in transaction encryption

#### KeyTree (HD key derivation)
- `KeyTreePublic` / `KeyTreePrivate` -- BTreeMap-based hierarchical key trees
- Chain indices for BIP-32-like derivation paths
- Layered node generation with 20-level depth cap
- Used by the wallet for multiple account management

#### NSSAUserData (the wallet's key manager)
- Manages all of the above: personal KeyChains, group key holders, shared accounts
- Handles public/private/shared account generation and lookup
- Serializable for persistent storage

### 3.2 Can a Module Generate Its Own KeyChain?

**Yes, absolutely.** `KeyChain::new_os_random()` is the exact API for this. It generates a complete identity (SSK, NPK, VPK) from OS entropy with zero dependencies on any other module or user interaction. This is the correct path for agent identity.

The LP-0008 spec says: "the agent holds its own shielded LEZ account (NPK/ISK keypair)." The `key_protocol` crate provides exactly this.

### 3.3 The LEZ Wallet CLI

The `wallet` crate in logos-execution-zone provides a full Rust wallet implementation:
- `WalletCore` struct with account management, transaction building, chain sync
- Uses `key_protocol` for all key operations
- Supports public accounts (transparent signing) and private accounts (ZK nullifier-based)
- Has `PersistentStorage` for key persistence across restarts
- Integrates with sequencer for transaction submission

**This is the code the agent wallet should wrap**, not reinvent. The wallet already knows how to:
- Create private/public accounts via KeyChain
- Build and submit shielded transactions
- Manage account state and nonces
- Poll for balance updates

---

## 4. Existing Agent Frameworks in the Logos Ecosystem

**None.** There is no agent framework, no A2A implementation, no skill dispatch system, no owner channel pattern in any official Logos repository.

### 4.1 Beach-Bum's Agora (Rejected LP-0008 Submission)

**Repo:** `Beach-Bum/agora-agent` (public, NOT archived)
**PR:** #34 in lambda-prize, closed 2026-04-28 without merge, no reviewer comments
**Architecture:** Dual-layer (C++/QML native module + Python standalone agent)

**What it built from scratch:**
- Python `AgentKeystore` class with OS keychain + encrypted file fallback (custom, NOT using key_protocol)
- On-chain identity registry (Rust SPEL contract) with secp256k1 pubkey as AgentId, NOM stake, CapabilityFlags bitmask
- On-chain escrow and reputation contracts
- 21-skill SDK across 5 categories
- A2A-compatible Agent Cards over Waku
- Owner channel with spending policy

**What went wrong (probable):**
1. **Identity is Python-native, not LEZ-native.** The keystore generates random 32-byte keys via `secrets.token_bytes(32)` and stores them in OS keychain. These are NOT KeyChain objects from `key_protocol`. The agent identity is completely disconnected from the LEZ key hierarchy.
2. **Wallet is a mock.** The "daemon_wallet.py" implements spending policy and audit trails in Python, but the actual on-chain interaction appears to go through the LEZ wallet CLI as a subprocess, not through native integration.
3. **The identity contract uses secp256k1 pubkeys directly**, but LP-0008 spec says "NPK/ISK keypair" -- implying the NSSA/LEZ-native key protocol, not raw secp256k1.
4. **No comments/reviews on the PR** -- silent rejection suggests fundamental approach issues, not just polish.

**Lessons for us:**
- MUST use `key_protocol::KeyChain` for agent identity, not roll our own
- MUST integrate with the LEZ wallet Rust crate directly, not wrap the CLI
- The Python standalone agent approach is valid but the native module is required
- Agora's skill system design (21 skills, 5 categories) matches the LP-0008 spec perfectly -- their categorization is a useful reference
- Agora's identity contract (CapabilityFlags, AgentRecord with stake/reputation) is interesting but NOT required by LP-0008 -- the spec asks for Agent Cards (A2A), not on-chain identity NFTs

---

## 5. Other Relevant Components

### WhisperWall (SPEL demo)
- Does NOT create its own keys -- relies on pre-existing LEZ wallet accounts
- Demonstrates PDA ownership, signer-based auth, chained calls, privacy cascading
- Useful as a SPEL contract reference, not an identity reference

### logos-chat-module
- Wraps `liblogoschat` with Qt signals
- Has "query client ID and identity info" capability
- Likely derives messaging identity from the account system, but source not documented
- Could be relevant for the owner channel implementation

### logos-waku-module
- C++ wrapper for Waku networking
- Identity handling not documented in public docs
- This is the transport layer for agent messaging

### logos-storage-module
- Has a `test_peerId` test suggesting P2P identity exists
- Storage module for Codex integration
- Identity details not publicly documented

### Scaffold Tool
- Seeds "a deterministic default wallet from preconfigured public accounts" during setup
- Has `lez-framework` template (archived, empty)
- No agent templates

---

## 6. Architectural Decision: What LP-0008 Builds vs. Reuses

### REUSE (from existing infrastructure):
| Component | Source | How |
|-----------|--------|-----|
| `KeyChain` for agent identity | `key_protocol` crate | `KeyChain::new_os_random()` at agent init |
| Private/public account creation | `key_protocol` | Via `SecretSpendingKey::produce_private_key_holder()` |
| Shielded transaction building | LEZ `wallet` crate | Wrap `WalletCore` or extract transaction builder |
| Persistent key storage | `wallet` crate's `PersistentStorage` | Same serialization format |
| SPEL contract patterns | WhisperWall example | PDA ownership, chained calls |
| Qt module interface | `logos-module-builder` | Standard module packaging |
| Waku transport | `logos-waku-module` | For owner channel + A2A messaging |
| Codex storage | `logos-storage-module` | For file upload/download skills |

### BUILD FROM SCRATCH:
| Component | Why |
|-----------|-----|
| Agent runtime | No existing agent lifecycle manager |
| Skill dispatch system | No existing skill framework |
| A2A protocol binding | No existing A2A implementation over Waku |
| Owner channel | No existing encrypted owner-agent channel |
| Spending approval state machine | No existing approval flow |
| Agent Card generation | No existing A2A card system |
| Agent-to-agent task lifecycle | No existing task orchestration |
| CLI deployment tool | No existing agent deployment tooling |

### DO NOT BUILD (Agora's mistakes to avoid):
| Component | Why |
|-----------|-----|
| Custom keystore (Python/JS) | Use `key_protocol::KeyChain` |
| On-chain identity NFT | LP-0008 asks for Agent Cards, not on-chain identity |
| On-chain reputation system | Not in LP-0008 requirements |
| Custom escrow contract | Not in LP-0008 requirements (spending threshold is local) |

---

## 7. Key Implications for Implementation

1. **The agent identity IS a KeyChain.** Call `KeyChain::new_os_random()`, persist the result. The NPK is the agent's public identity. The VPK is for encrypted note receipt. The SSK is for transaction signing.

2. **The agent wallet wraps the LEZ wallet crate.** Don't build transaction construction from scratch. The `WalletCore` already handles shielded transfers, chain sync, and account state management.

3. **The capability module is irrelevant to LP-0008.** It handles inter-module permissions at the Qt plugin level, not agent-level capabilities. Our agent's capabilities are expressed through A2A Agent Cards.

4. **The accounts module is irrelevant.** It wraps go-wallet-sdk for Ethereum-style accounts. LP-0008 needs LEZ-native NSSA accounts via `key_protocol`.

5. **Agora was rejected but its repo is public.** Study its architecture as a reference (especially skill categorization and A2A topic structure) but do NOT copy its identity approach. The Python keystore is fundamentally wrong for LP-0008.

6. **The field is clear.** No competing submissions are open. Agora's rejection means the evaluators have seen one bad approach and know what they don't want. Our advantage: native Rust integration with `key_protocol` from day one.
