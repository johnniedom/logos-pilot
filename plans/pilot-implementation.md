# Plan: Pilot (LP-0008 Sovereign Agent on LEZ) — Revised

> Previous plan treated phases as "done" when code compiled. This revision distinguishes **skeleton** (compiles, passes unit tests) from **production** (works end-to-end against real Logos modules).
>
> All reference docs in `docs/research/`: spec.md, decisions.md, research-hard-problems.md, audit-module-apis.md, research-identity-infrastructure.md

---

## Current State (updated 2026-05-20 evening)

### What exists, works, and is verified live
- C++ Logos Core module: 37 methods, builds via `nix build` (regular + portable + LGX)
- **Confirmed running headlessly via logoscore CLI** (`logos-co/logos-logoscore-cli`)
- `echo()`, `metaSkills()`, `metaStatus()`, `processOwnerMessage()` all return correct results live
- 44 unit tests pass: crypto round-trips, skill registry, LLM factory, owner message routing
- 21 skills registered through `SkillRegistry` with names, categories, descriptions, pricing
- LLM Provider: `AnthropicProvider` + `OpenAIProvider` + `NoOpProvider` fallback
- AES-256-GCM file encryption: encrypt before upload, decrypt after download (unit tested)
- ECIES encryption: secp256k1 ECDH + SHA256 KDF + AES-GCM for A2A and messaging (unit tested)
- `pilot` CLI: deploy wizard (arrow-key selector), chat REPL, verify, discover, configure, status
- Skill interface: `PilotSkill` abstract class, `LambdaSkill`, `SkillRegistry`, example weather skill
- 7 docs written: architecture, security-model, payment-model, owner-guide, deployment-guide, skill-interface, DEVELOPER_GUIDE
- SQLite WAL persistence (identity, owner channel, spend requests, stored files, config)
- 4-tab QML Basecamp UI plugin (Dashboard, Chat, Wallet, Skills)

### What is code-complete but NOT integration-tested against real Logos modules
| Component | What's needed |
|-----------|--------------|
| Identity creation | Test `lez_wallet_module.createAccountPrivate()` with real wallet module |
| Owner channel | Test `chat_module.requestMyBundle()` + `createConversation()` with real chat |
| Spending FSM | Test full 9-state flow with real `transferPrivate()` on-chain |
| Storage encryption | Test AES-256-GCM encrypt→upload→download→decrypt with real storage_module |
| File sharing | Test ECIES key encryption + delivery with real delivery_module |
| A2A transport | Test ECIES-encrypted task request/reply between two pilot instances |
| LLM integration | Test with real `ANTHROPIC_API_KEY` for natural language command routing |
| Crash recovery | Test `recoverPendingTransactions()` after kill/restart |

### What is still missing (spec requires it)
| Required by spec | Status |
|-----------------|--------|
| 3 narrated demo videos | **Not started** (Phase F) |
| 5 third-party deployments on LEZ testnet | **Not started** (Phase G) |
| CU cost documentation per on-chain operation | **Not started** (Phase F — needs live testing) |
| Reproducible `demo.sh` against real local sequencer | **Script exists, not verified against real infra** |
| CI green on default branch | **Not set up** |
| End-to-end integration tests in CI | **Not set up** |

---

## Architectural Decisions (carried forward from `decisions.md`)

These are locked. Do not revisit:

- **D1**: Separate repo `johnniedom/pilot` with `pilot-module/` (C++) and `pilot-ui/` (QML)
- **D2**: `LLMProvider` trait. Ship `AnthropicProvider` + `OpenAIProvider`. Arrow-key CLI selector. Config via env vars.
- **D3**: Trait-based skill interface (C++ abstract class). Third parties implement + list in config.
- **D4**: SQLite WAL for spending persistence. Already implemented.
- **D5**: Text-command chat via chat_module E2E. Auto-establish via `requestMyBundle()`. Already implemented (untested).
- **D6**: A2A `WakuTransport` adapter. NATS-style reply topics. Already implemented (untested).
- **D7**: Pay-on-acceptance for v1. Document escrow as v2 path.
- **D11**: Qt Remote Objects for all inter-module calls (not FFI). Already implemented.
- **D12**: chat_module for owner channel, ECIES for A2A messages.
- **D13**: AES-256-GCM per-file encryption. Key stored in SQLite. Share = ECIES key to recipient NPK.
- **D14**: Waku Store via waku_module for crash recovery message replay.
- **D15**: Depend on both delivery_module AND waku_module.

---

## Phase A: LLM Provider (the intelligence layer) — COMPLETED 2026-05-20

**Why first**: Without this, Pilot is a dumb RPC dispatcher, not an AI agent. The spec says "pluggable inference" and the prototype shows LLM as a core architecture component. The agent needs to understand natural language owner commands and make decisions about task delegation.

### What to build

**`pilot_llm.h` — Abstract LLM provider interface:**

```cpp
class LLMProvider {
public:
    virtual ~LLMProvider() = default;
    virtual std::string complete(const std::string& prompt) = 0;
    virtual std::string model() const = 0;
    virtual bool isConfigured() const = 0;
};
```

**`pilot_llm_anthropic.cpp` — AnthropicProvider:**
- HTTP POST to `https://api.anthropic.com/v1/messages`
- Headers: `x-api-key`, `anthropic-version: 2023-06-01`, `content-type: application/json`
- Uses Qt `QNetworkAccessManager` for HTTP (available in module context)
- Config from env: `ANTHROPIC_API_KEY`, `PILOT_LLM_MODEL` (default: `claude-sonnet-4-6`)

**`pilot_llm_openai.cpp` — OpenAIProvider:**
- HTTP POST to `${OPENAI_BASE_URL}/chat/completions` (default: `https://api.openai.com/v1`)
- Headers: `Authorization: Bearer ${OPENAI_API_KEY}`, `content-type: application/json`
- Covers: GPT, Gemini (via Google's OpenAI-compatible endpoint), Ollama, LM Studio, OpenRouter
- Config from env: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `PILOT_LLM_MODEL`

**`pilot_llm_factory.cpp` — Provider factory:**
- Reads `PILOT_LLM_PROVIDER` env var (values: `anthropic`, `openai`)
- Creates the appropriate provider instance
- Falls back to no-op provider if not configured (agent still works for hardcoded commands)

**Integration with owner channel:**
- When owner sends free-text (not a `/command`), pass to LLM with context
- LLM returns structured intent: `{"action": "approve", "id": "a3f7"}` or `{"action": "delegate", "agent": "LinguaBot", "skill": "translate", "params": {...}}`
- Keep LLM system prompt minimal: list available commands + current state

### Files to create
- `pilot-module/src/pilot_llm.h` — abstract interface
- `pilot-module/src/pilot_llm_anthropic.cpp`
- `pilot-module/src/pilot_llm_openai.cpp`
- `pilot-module/src/pilot_llm_factory.cpp`

### Files to modify
- `pilot-module/src/pilot_impl.h` — add `LLMProvider* llm_` member, `configureLLM()` method
- `pilot-module/src/pilot_impl.cpp` — init LLM in `initialize()`
- `pilot-module/src/pilot_owner.cpp` — route non-command messages through LLM
- `pilot-module/src/pilot_meta.cpp` — `metaStatus()` reports LLM provider/model
- `pilot-module/CMakeLists.txt` — add new source files, link OpenSSL for HTTPS

### Acceptance criteria
- [ ] `metaStatus()` reports configured LLM provider and model name
- [ ] `metaConfigure("llm.provider", "anthropic")` switches provider at runtime
- [ ] `metaConfigure("llm.model", "claude-sonnet-4-6")` switches model at runtime
- [ ] Owner sends "what's my balance?" → LLM interprets → agent calls `walletBalance()` → returns result
- [ ] Owner sends "yeah go ahead" after approval card → LLM maps to `/approve`
- [ ] Agent works without LLM configured (falls back to command-only mode)
- [ ] Unit test: provider factory creates correct type from env var
- [ ] Unit test: AnthropicProvider formats request JSON correctly
- [ ] Unit test: OpenAIProvider formats request JSON correctly

---

## Phase B: Real Encryption — COMPLETED 2026-05-20

**Why second**: The spec says "encrypts and uploads" for storage, "end-to-end encrypted" for messaging. Our current code fakes this entirely. Without real encryption, evaluators can trivially see that files are uploaded in cleartext.

### What to build

**AES-256-GCM file encryption (storage skills):**
- Use OpenSSL EVP API (`EVP_aes_256_gcm`)
- Generate random 256-bit key + 96-bit IV per file
- Encrypt file content before passing to storage_module `uploadInit/uploadChunk/uploadFinalize`
- Store `{key, iv, tag}` in SQLite `stored_files.file_key_encrypted` column (hex-encoded)
- Decrypt after `downloadFile` retrieval

**ECIES for A2A messages (inter-agent encryption):**
- Generate ephemeral EC keypair per message
- ECDH with recipient's NPK to derive shared secret
- AES-256-GCM encrypt payload with derived key
- Send: `{ephemeral_pubkey, ciphertext, iv, tag}`
- Recipient uses their SSK + ephemeral pubkey to derive same shared secret and decrypt

**Where encryption applies (from audit-module-apis.md D13):**
| Topic | Encryption | Implementation |
|-------|-----------|----------------|
| `/pilot/1/discovery/proto` | Plaintext (cards are public) | No change needed |
| `/pilot/1/inbox-{hash}/proto` | ECIES to agent NPK | New: encrypt before `delivery.send()` |
| `/pilot/1/reply-{uuid}/proto` | ECIES to sender NPK | New: encrypt before `delivery.send()` |
| `/pilot/1/owner-{id}/proto` | E2E via chat_module | Already handled by chat_module |
| Storage files | AES-256-GCM per-file | New: encrypt before upload |

### Files to create
- `pilot-module/src/pilot_crypto.h` — AES-256-GCM + ECIES functions
- `pilot-module/src/pilot_crypto.cpp`

### Files to modify
- `pilot-module/src/pilot_storage.cpp` — encrypt before upload, decrypt after download
- `pilot-module/src/pilot_a2a.cpp` — ECIES wrap/unwrap on inbox and reply messages
- `pilot-module/src/pilot_messaging.cpp` — ECIES encrypt outgoing messages
- `pilot-module/CMakeLists.txt` — link OpenSSL (`find_package(OpenSSL REQUIRED)`)
- `pilot-module/metadata.json` — add OpenSSL to nix cmake find_packages if needed

### Acceptance criteria
- [ ] `storageUpload()` encrypts file content — hex dump of uploaded bytes is random, not plaintext
- [ ] `storageDownload()` decrypts — retrieved file matches original byte-for-byte
- [ ] `storageShare()` ECIES-encrypts the file key to recipient's NPK
- [ ] A2A inbox messages are ECIES-encrypted — eavesdropper sees only ciphertext
- [ ] A2A reply messages are ECIES-encrypted to sender's NPK
- [ ] Discovery messages remain plaintext (Agent Cards are public)
- [ ] Unit test: AES-256-GCM encrypt→decrypt round-trip
- [ ] Unit test: ECIES encrypt→decrypt round-trip with known keypair

---

## Phase C: `pilot` CLI Tool — COMPLETED 2026-05-20

**Why third**: The spec explicitly requires "a CLI for agent deployment, configuration, and initial funding" and "single CLI command" deployment. Evaluators will run this.

### What to build

**`pilot deploy --testnet` interactive wizard:**

The prototype's Get Started tab (steps 0-6) defines the exact flow:

1. **Agent Identity** — generate keypair via `KeyChain::new_os_random()` (calls lez_wallet_module)
2. **LLM Provider** — arrow-key selector: Claude, OpenAI/GPT, Gemini, Local (Ollama), OpenRouter
3. **Owner Identity** — input owner's Logos address (from Basecamp > Settings)
4. **Funding** — show agent's address, wait for deposit, confirm balance
5. **Deploy** — start the module, publish Agent Card, open owner channel

**Arrow-key selector** (D2 — inquirer/dialoguer style, NOT numbered input):
- Render options with `›` indicator on active option
- Arrow up/down navigates, Enter confirms
- Same UX as prototype's Step 2

**`pilot verify` evidence output:**
- Agent address + NPK
- Current balance
- Transaction hashes (last 10)
- Skill status (all 21)
- Agent Card published (yes/no)
- Uptime
- Peer agents discovered
- Format: JSON for machine parsing + human-readable table

**`pilot discover`:**
- Query `/pilot/1/discovery/proto` via delivery_module
- List discovered agents with skills and pricing

### Implementation approach

The CLI is a **separate executable** (not part of the Logos Core module). Two options:

**Option 1: Bash script with escape codes** — simpler, no compile step, works everywhere with bash
- Arrow-key selector via ANSI escape codes + `read -rsn1`
- Calls `logoscore` commands or directly invokes module methods
- Evidence collection via module API calls

**Option 2: Go/Rust binary** — better UX, cross-platform, proper error handling
- Use `dialoguer` (Rust) or `survey` (Go) for arrow-key selectors
- Communicates with running Pilot module via Qt Remote Objects or local HTTP

**Recommendation: Bash script for v1** — faster to ship, evaluators run on Linux, matches nix ecosystem. Package in the `.lgx` or install alongside module.

### Files to create
- `pilot-cli/pilot` — main CLI script
- `pilot-cli/lib/arrow-select.sh` — arrow-key selector helper
- `pilot-cli/lib/deploy.sh` — deployment wizard
- `pilot-cli/lib/verify.sh` — evidence collection

### Acceptance criteria
- [ ] `pilot deploy --testnet` walks through all 5 steps interactively
- [ ] Arrow-key selector works for LLM provider choice (up/down/enter)
- [ ] After deployment: module running, Agent Card published, owner channel open
- [ ] `pilot verify` outputs machine-parseable evidence JSON
- [ ] `pilot discover` lists discovered peer agents
- [ ] `pilot --help` shows all available commands
- [ ] `pilot configure llm.model claude-opus-4-6` changes LLM model at runtime
- [ ] Fresh clone + `pilot deploy --testnet` works within 5 minutes (after nix cache exists)

---

## Phase D: Third-Party Skill Interface — COMPLETED 2026-05-20

**Why fourth**: Spec requires "a documented skill interface (module/SDK) that can be used to add new skills without modifying the core agent module."

### What to build

**Skill trait (C++ abstract class):**

```cpp
class PilotSkill {
public:
    virtual ~PilotSkill() = default;
    virtual std::string name() const = 0;
    virtual std::string category() const = 0;
    virtual std::string description() const = 0;
    virtual std::string inputSchema() const = 0;   // JSON Schema
    virtual std::string outputSchema() const = 0;   // JSON Schema
    virtual int64_t priceLez() const { return 0; }  // 0 = free
    virtual std::string execute(const std::string& argsJson) = 0;
};
```

**Skill registry:**
- `registerSkill(std::unique_ptr<PilotSkill>)` — add a skill at runtime
- `listSkills()` — returns all registered skills (feeds `metaSkills()`)
- `dispatchSkill(name, args)` — lookup + execute by name
- Built-in skills register during `initialize()`
- Third-party skills loaded from shared libraries (`.so`) in a plugins directory

**Example third-party skill:**
- `examples/skill-weather/` — simple weather lookup skill
- Shows: implementing the trait, building as `.so`, installing into Pilot
- Documented in `docs/skill-interface.md`

### Files to create
- `pilot-module/src/pilot_skill.h` — abstract PilotSkill class + SkillRegistry
- `pilot-module/src/pilot_skill_registry.cpp` — registry implementation
- `examples/skill-weather/` — example third-party skill
- `docs/skill-interface.md` — documentation for third-party developers

### Files to modify
- `pilot-module/src/pilot_impl.h` — add SkillRegistry member
- `pilot-module/src/pilot_impl.cpp` — register built-in skills in `initialize()`
- `pilot-module/src/pilot_meta.cpp` — `metaSkills()` reads from registry

### Acceptance criteria
- [ ] All 21 built-in skills registered through the skill interface (not hardcoded dispatch)
- [ ] Third-party skill `.so` loaded from plugins directory at startup
- [ ] `metaSkills()` includes both built-in and third-party skills
- [ ] Example weather skill builds, installs, and appears in skill list
- [ ] `docs/skill-interface.md` covers: trait definition, build instructions, installation, A2A schema generation
- [ ] Agent Card `skills` array auto-generated from registry

---

## Phase E: Integration Verification

**Why fifth**: Everything before this was building components. Now we verify they work together against real Logos modules on LEZ testnet.

### What to verify

**Identity (Phase 1 code, now tested for real):**
- [ ] `initialize()` → `createAccountPrivate()` returns real account ID from lez_wallet_module
- [ ] `walletBalance()` returns real balance (should be 0 on fresh account)
- [ ] `getAgentNpk()` returns a valid NPK that other modules can encrypt to
- [ ] Identity persists across module restart (SQLite row survives)

**Owner channel (Phase 2 code, now tested for real):**
- [ ] `establishOwnerChannel()` → `requestMyBundle()` + `createConversation()` succeeds
- [ ] Owner sees conversation in Basecamp Chat
- [ ] Agent receives messages via `chatNewMessage` event
- [ ] Agent sends messages via `sendMessage(conversationId, content)`

**Spending FSM (Phase 3 code, now tested for real):**
- [ ] Below-threshold: `walletSend()` → `transferPrivate()` executes without approval
- [ ] Above-threshold: `walletSend()` → creates HELD → sends approval card → waits
- [ ] `/approve` → APPROVED → EXECUTING → COMPLETED (real on-chain tx)
- [ ] `/reject` → REJECTED (hold released)
- [ ] Crash recovery: kill module with pending NOTIFIED tx → restart → re-sends notification

**Storage (Phase 4 code + Phase B encryption):**
- [ ] Upload: real file → AES-256-GCM encrypt → storage_module upload → CID returned
- [ ] Download: CID → storage_module download → AES-256-GCM decrypt → file matches original
- [ ] List: shows all uploaded files with CIDs
- [ ] Share: ECIES-encrypts key to recipient NPK, sends via delivery_module

**A2A (Phase 5 code + Phase B encryption):**
- [ ] Agent Card published to `/pilot/1/discovery/proto` (plaintext, ISK-signed)
- [ ] `agentDiscover()` fetches cards from discovery topic
- [ ] `agentTask()` sends ECIES-encrypted request to peer's inbox
- [ ] Reply received on ephemeral reply topic
- [ ] Full task lifecycle: submitted → working → completed + LEZ payment

**LLM (Phase A code):**
- [ ] Natural language command → LLM interprets → correct skill dispatched
- [ ] LLM fallback: "yeah go ahead" → maps to `/approve`

### Evidence to collect during testing
- Transaction hashes for CU cost documentation
- Screenshots/logs for demo videos
- Agent addresses for third-party deployment instructions

---

## Phase F: Demo & Documentation

**What the spec requires:**
1. Three narrated demo videos (not silent screencasts)
2. Architecture docs, skill interface spec, security model
3. Reproducible `demo.sh` with `RISC0_DEV_MODE=0` (only when proving SPEL guest binary)
4. CU cost documentation per on-chain operation

### Demo videos (from D9)

**Video 1 (5-7 min): Personal File Vault**
- Owner sends file via Basecamp Chat → agent encrypts → uploads to Codex → returns CID
- Owner retrieves from second device
- Shows: storage skills, AES-256-GCM encryption, owner channel

**Video 2 (5-7 min): On-Chain Event Alerter**
- Agent monitors a LEZ program for state change
- Change detected → Waku message → Basecamp Chat alert to owner
- Shows: blockchain skills, messaging, owner channel

**Video 3 (8-10 min): Multi-Agent Paid Task**
- Agent A discovers Agent B via Agent Card on discovery topic
- A requests translation task from B
- B accepts → A pays LEZ → B completes → result returned
- Shows: A2A protocol, ECIES encryption, LEZ payment, task lifecycle

### Documentation to write
- `docs/architecture.md` — system architecture (matches prototype's Architecture tab)
- `docs/skill-interface.md` — third-party skill development guide
- `docs/payment-model.md` — pay-on-acceptance v1, escrow v2 path
- `docs/security-model.md` — what agent can/cannot do without owner approval
- `docs/deployment-guide.md` — step-by-step deployment + `pilot deploy` reference
- `docs/owner-guide.md` — owner interaction commands, approval flow
- `docs/cu-benchmarks.md` — CU cost per on-chain operation (measured during Phase E)

### Acceptance criteria
- [ ] 3 videos uploaded (narrated, showing terminal output)
- [ ] Video 3 shows `RISC0_DEV_MODE=0` terminal output during proof generation
- [ ] `scripts/demo.sh` works from clean clone against local sequencer
- [ ] All 7 docs written and accurate
- [ ] README covers end-to-end: deploy, configure, interact, extend

---

## Phase G: Third-Party Deployments & Submission

**Why last**: Requires working software + good deployment UX.

### The 5-deployment formula (from research-hard-problems.md)

1. **Docker Compose quickstart** — Waku + Codex + LEZ in one `docker compose up`
2. **`pilot deploy --testnet --config examples/file-vault.toml`** — 5-minute install
3. **DM 15-20 people in Logos Discord** — target #builder-hub
4. **Hand-hold first 2** — they are QA, find bugs in the deployment UX
5. **`pilot verify`** — automatic evidence output (agent address, tx hashes, skill status)
6. **Budget $100 for bounties** — $20 USDC each, activate only if needed

### Evidence package
- Table: 5 agent addresses + transaction hashes
- `pilot discover` output showing 5+ agents
- GitHub Issues: each deployer opens issue with `pilot verify` output
- On-chain records from LEZ testnet

### Submission
- Push to `johnniedom/pilot` (public, MIT license)
- PR to `logos-co/lambda-prize` repo `solutions/` directory with `solutions-LP-0008.md`
- Max 3 submissions, 1 per week

### Acceptance criteria
- [ ] 5 agents deployed on LEZ testnet by outside parties
- [ ] Each demonstrates at least one skill autonomously
- [ ] Evidence package assembled
- [ ] Public repo with MIT/Apache-2.0 license
- [ ] Submission PR created

---

## Phase Order Summary

| Phase | What | Status | Effort |
|-------|------|--------|--------|
| **A** | LLM Provider | **COMPLETED** 2026-05-20 | Done in 1 session |
| **B** | Real Encryption | **COMPLETED** 2026-05-20 | Done in 1 session |
| **C** | `pilot` CLI | **COMPLETED** 2026-05-20 | Done in 1 session |
| **D** | Skill Interface | **COMPLETED** 2026-05-20 | Done in 1 session |
| **E** | Integration Verification | **NEXT** — needs dependency modules installed | 2-3 days |
| **F** | Demo & Docs | Docs written, videos not started | 2-3 days |
| **G** | Deployments & Submit | Not started | 5-7 days (waiting on people) |

**Completed:** Phases A-D (all code work) in one session.
**Remaining:** Phases E-G (integration, demos, deployments) — ~10-13 days.

Start Phase G outreach (writing quickstart guide, Docker Compose) in parallel with Phase E — don't wait until code-complete.

---

## Key Risks (updated)

1. ~~**LLM HTTP calls from C++ module**~~ — **RESOLVED.** QNetworkAccessManager + QEventLoop works inside the logos_host subprocess.
2. ~~**OpenSSL availability in nix build**~~ — **RESOLVED.** `find_package(OpenSSL REQUIRED)` works, OpenSSL 3.5.1 available.
3. **ECIES with LEZ key types** — NPK from key_protocol may not be a standard EC public key. Need to verify the exact curve and format during Phase E integration testing.
4. **waku_module storeQuery** — not in current dependencies. May need to re-add for crash recovery message replay.
5. **RISC0 proof generation** — only needed for Video 3. Must have a SPEL guest binary to prove.
6. **Third-party deployer conversion rate** — budget time for the $100 bounty fallback.
7. **logoscore CLI vs liblogos** — **RESOLVED.** The real CLI is `logos-co/logos-logoscore-cli` (daemon + inline mode). The old `logos-liblogos-build` binary has no CLI parsing. Modules must be installed via `lgpm` (LGX packages), not bare `.so` files.
