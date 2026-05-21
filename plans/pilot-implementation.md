# Plan: Pilot (LP-0008 Sovereign Agent on LEZ) — Final

> Updated 2026-05-21 after Phase E integration verification complete.

---

## Current State (2026-05-21)

### Fully working and tested
- 41 features PASS across all phases (identity, wallet, owner channel, spending FSM, storage, messaging, A2A, meta)
- 44 unit tests PASS (72ms)
- CLI interactive chat working in daemon mode (`pilot chat`)
- ECIES encryption on owner channel + messaging + file sharing + A2A
- Real Waku network messaging — connected to 6 Logos relay peers (Amsterdam, US-Central, Hong Kong)
- Encrypted file storage with CIDs via Codex storage module
- Full A2A Agent Card with 9 skills, pricing, authentication published
- Comprehensive documentation (19 sections in findings-integration.md)

### Key architectural decisions made during integration
- **Owner channel uses delivery_module** (not chat_module) — chat_module's callMethod dispatch is empty, delivery_module is Qt-wrapped with auto-dispatch and working Waku transport
- **Agent generates own ECIES keypair** — separate from wallet keys (wallet-ffi only exposes public keys)
- **Storage configured with `{"nat":"none"}`** — disables UPnP for WSL compatibility
- **Waku cluster-id=2, 8 shards** — matches Logos delivery network configuration
- **LogosResult extraction** — storage module returns custom Qt type, extracted via `result.value<LogosResult>().value`

---

## What's done

| Phase | Status |
|-------|--------|
| Phase 0: Infrastructure | DONE — builds, loads 6 modules, 44 unit tests |
| Phase A: LLM Provider | DONE — Anthropic + OpenAI + NoOp fallback |
| Phase B: Real Encryption | DONE — AES-256-GCM + ECIES secp256k1 |
| Phase C: pilot CLI | DONE — deploy, chat, verify, discover, configure, status |
| Phase D: Skill Interface | DONE — 21 skills, PilotSkill trait, SkillRegistry |
| Phase E: Integration | DONE — 41 features tested against real modules |
| Phase 1: Identity + Wallet | DONE — wallet-ffi keys, persistence |
| Phase 2: Owner Channel | DONE — ECIES via delivery_module, Waku relay |
| Phase 3: Spending FSM | DONE — 9-state machine, SQLite persistence |
| Phase 4: Storage + Messaging | DONE — upload/share/send all working |
| Phase 5: A2A Transport | DONE — agent cards, task protocol, discovery |
| CLI Interactive | DONE — daemon mode, REPL, stale state recovery |

---

## What's left

### Priority 1: Code fixes (do now)

| Item | What | Time |
|------|------|------|
| Per-period spending limit | `walletSend()` needs to total spending in current period before auto-approving | 20 min |
| Request expiry | Check `expires_at` in `approveSpend()`, reject expired requests | 20 min |
| listSkillsForCard() swap | Name/description fields swapped in agent card skill list | 2 min |
| README cleanup | Remove stale "placeholder encryption" language — encryption is real now | 5 min |

### Priority 2: CI pipeline

| Item | What | Time |
|------|------|------|
| GitHub Actions workflow | `nix build` + `nix build .#unit-tests` on push | 30 min |

### Priority 3: Demo evidence

| Item | What | Time |
|------|------|------|
| Two-agent task | Two logoscore instances discover each other, complete a task, transfer LEZ | 1-2 hours |
| 3 use case demos | Record CLI sessions showing file vault, agent marketplace, multi-agent workflow | 2 hours |
| CU cost documentation | Run on-chain operations, record compute unit costs from sequencer | 1 hour |

### Priority 4: External (needs people/time)

| Item | What | Time |
|------|------|------|
| 5 third-party deployments | Recruit 5 people to deploy pilot on testnet, collect evidence | Days |
| Submission PR | PR to logos-co/lambda-prize with solutions-LP-0008.md | 1 hour |

### Not fixable locally

| Item | Why |
|------|-----|
| Storage download | Local node has no peers — needs connected storage network |
| programCall/Query | wallet-ffi v0.1 doesn't support program methods — LEZ v0.2.0-rc3 feature |

---

## Bugs found and fixed (this session)

| # | Bug | Fix |
|---|-----|-----|
| 1 | `logosAPI_` always NULL | Inject `onInit(LogosAPI*)` override via sed in flake.nix |
| 2 | capability_module stub crashes | Install real `.so` from nix store |
| 3 | SQLite dir not created | `mkdir()` before `sqlite3_open()` |
| 4 | Wallet camelCase methods | `getBalance` → `get_balance`, `transferPrivate` → `transfer_private` |
| 5 | Wallet config missing field | Added `"initial_accounts": []` |
| 6 | Wallet amount format | Plain int → 32-char LE hex string |
| 7 | Spending FSM false success | Check result string for "fail"/"error" |
| 8 | chat_module callMethod empty | Switched to delivery_module for all messaging |
| 9 | Storage NAT timeout | `{"nat":"none"}` config |
| 10 | Storage LogosResult extraction | `result.value<LogosResult>().value` |
| 11 | Storage download method name | `downloadFile` → `downloadChunks` |
| 12 | Owner channel plaintext | ECIES encryption with owner public key |
| 13 | Owner channel one-way | Added incoming message event listener + decrypt + processOwnerMessage |
| 14 | Waku topic 5 segments | Changed to 4-segment format |
| 15 | Waku missing cluster/shards | Added `clusterId:2, shards:[0..7]` |
| 16 | Waku peer port wrong | Container port 60000 → host port 30303 |
| 17 | CLI daemon stale state | Detect dead PID, clean state, restart fresh |

---

## Commits this session

```
a612e94 fix: pass topic argument to agentDiscover in CLI
aa75e6a fix: clean stale daemon state on startup
da03111 fix: CLI stability — poll-based daemon startup, REPL resilience
80383f3 fix: CLI REPL crash on first command + empty NPK
2931bcd feat: wire pilot CLI to real module paths and daemon mode
8e7f230 docs: comprehensive integration findings — all discoveries documented
d8d80e4 test: verify programCall/programQuery — wallet-ffi v0.1 limitation
0ae3150 fix: Waku content topic format + storage download LogosResult handling
7f5caaa docs: update architecture to reflect delivery_module owner channel
6c47f44 feat: owner channel via delivery_module — messages on Waku network
871886d feat: ECIES-encrypted bidirectional owner channel
ec89a90 fix: storage upload end-to-end — LogosResult extraction + NAT config
67e3821 feat: auto-init dependency modules + fix wallet amount format
80dd0ef feat: event-based owner channel + fix storage/spending error handling
51e9307 fix: wallet snake_case method names and document integration test results
fe2afd1 chore: clean up scripts — remove debug, rename setup-modules
e1a83d9 feat: Phase E — integration verification with real modules
```
