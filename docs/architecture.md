# Pilot Agent — Architecture

Ref: [LP-0008 Spec](research/spec.md) | [Decisions](research/decisions.md) | [Module Builder](https://github.com/logos-co/logos-module-builder)

## Overview

Pilot is a Logos Core universal C++ module that turns the Logos stack into an autonomous AI agent. It has its own identity, wallet, and communication channels — operating as a first-class participant in the Logos ecosystem.

```
┌─────────────────────────────────────────────────────┐
│                    Basecamp / logoscore              │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────┐ │
│  │lez_wallet│ │ delivery │ │ storage  │ │  chat  │ │
│  │ _module  │ │ _module  │ │ _module  │ │_module │ │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └───┬────┘ │
│       │            │            │            │      │
│       └────────────┴─────┬──────┴────────────┘      │
│                    LogosAPI (Qt Remote Objects)      │
│                          │                          │
│              ┌───────────┴───────────┐              │
│              │     Pilot Module      │              │
│              │  ┌─────┐ ┌─────────┐  │              │
│              │  │ LLM │ │ Crypto  │  │              │
│              │  └─────┘ └─────────┘  │              │
│              │  ┌─────┐ ┌─────────┐  │              │
│              │  │Skills│ │ SQLite  │  │              │
│              │  └─────┘ └─────────┘  │              │
│              └───────────────────────┘              │
└─────────────────────────────────────────────────────┘
         │                        │
    ┌────┴────┐              ┌────┴────┐
    │pilot CLI│              │ QML UI  │
    │(deploy, │              │(Basecamp│
    │chat,    │              │ plugin) │
    │verify)  │              └─────────┘
    └─────────┘
```

## Module Structure

```
pilot-module/src/
├── pilot_impl.h             # Public interface (pure C++, 37 methods)
├── pilot_impl.cpp           # Core: constructor, DB schema, LLM routing
├── pilot_identity.cpp       # Identity creation, wallet setup, config loading
├── pilot_owner.cpp          # Owner channel via delivery_module + ECIES
├── pilot_spending.cpp       # 9-state spending FSM with SQLite persistence
├── pilot_storage.cpp        # AES-256-GCM encrypted upload/download/share
├── pilot_messaging.cpp      # ECIES-encrypted messaging
├── pilot_a2a.cpp            # A2A protocol: Agent Cards, tasks, discovery
├── pilot_meta.cpp           # Status, skills listing, runtime config
├── pilot_llm.h              # Abstract LLM provider interface
├── pilot_llm_anthropic.cpp  # Claude API provider
├── pilot_llm_openai.cpp     # OpenAI-compatible provider (GPT, Gemini, Ollama)
├── pilot_llm_factory.cpp    # Auto-detect provider from env vars
├── pilot_crypto.h           # AES-256-GCM + ECIES function signatures
├── pilot_crypto.cpp         # OpenSSL EVP implementation
├── pilot_skill.h            # PilotSkill abstract class + SkillRegistry
├── pilot_skill_registry.cpp # Registry: register, list, dispatch
└── pilot_builtin_skills.cpp # 21 built-in skills as LambdaSkill wrappers
```

## Inter-Module Communication

All module calls go through Qt Remote Objects via LogosAPI (Decision D11):

```cpp
auto* wallet = logosAPI_->getClient("lez_wallet_module");
QVariant result = wallet->invokeRemoteMethod(
    "lez_wallet_module", "getBalance",
    QString::fromStdString(accountId), QVariant(false));
```

Dependencies (declared in `metadata.json`):
- `lez_wallet_module` — shielded accounts, transfers, program calls
- `delivery_module` — Waku message delivery (owner channel, A2A, messaging skills)
- `storage_module` — chunked file upload/download (Codex)
- `chat_module` — listed but bypassed (see [Owner Channel](owner-channel.md))
- `waku_module` — Store queries for crash recovery

## Encryption Model

| Channel | Encryption | Implementation |
|---------|-----------|----------------|
| Owner channel | ECIES to owner NPK via delivery_module | `pilot_crypto.cpp` ([details](owner-channel.md)) |
| Agent inbox (`/pilot/1/inbox-{npk}/proto`) | ECIES to agent NPK | `pilot_crypto.cpp` |
| A2A reply topics | ECIES to sender NPK | `pilot_crypto.cpp` |
| Discovery topic | Plaintext (Agent Cards are public) | No encryption |
| Stored files | AES-256-GCM per-file | `pilot_crypto.cpp` |
| Shared file keys | ECIES to recipient NPK | `pilot_crypto.cpp` |

ECIES flow: ephemeral EC keypair (secp256k1) → ECDH with recipient NPK → SHA256 KDF → AES-256-GCM.

## LLM Integration

Pluggable inference via abstract `LLMProvider` class:

```
Owner message → is it a /command? → yes → dispatch directly
                                  → no  → LLM.complete(system_prompt, message)
                                          → structured JSON action
                                          → dispatch skill
```

Providers: `AnthropicProvider` (Claude API), `OpenAIProvider` (OpenAI, Gemini, Ollama, LM Studio, OpenRouter). Falls back to `NoOpProvider` (command-only mode) if no API key configured.

## Skill Interface

All 21 skills registered through `SkillRegistry` at construction. Third parties implement `PilotSkill` abstract class and drop `.so` into plugins directory.

See [Skill Interface Documentation](skill-interface.md).

## Persistence

SQLite WAL mode (`pilot.db`):
- `agent_identity` — NPK, account ID (singleton row)
- `owner_channel` — conversation ID (singleton row)
- `spend_requests` — full FSM state for each request
- `stored_files` — CID, label, encrypted AES key (hex)
- `config` — key-value pairs (LLM provider, spending limits, etc.)

## Testing

- **44 unit tests** via logos-test-framework (`nix build .#unit-tests`)
- **logoscore CLI** for headless integration testing (`github:logos-co/logos-logoscore-cli`)
- **Basecamp** for full GUI integration
