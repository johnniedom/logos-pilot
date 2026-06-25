# Plan: Pilot (LP-0008 Sovereign Agent on LEZ) — Final

> Updated **2026-06-24**. Supersedes the 2026-05-21 snapshot.
> The authoritative, honest status lives in [`KNOWN_LIMITATIONS.md`](../KNOWN_LIMITATIONS.md)
> and the CI runs; this plan tracks scope and what remains. Historical detail from the
> integration phase is preserved in the appendix.

---

## Current State (2026-06-24)

### Verified in CI — green on `feat/sovereign-funding` (HEAD `1bff68e`)
Three jobs pass: **Build C++ Module**, **Build Nim CLI**, **E2E**.

- Module **builds**; **109 unit tests pass**; **E2E passes** — pilot + its dependencies load
  into the `logoscore` runtime against a **standalone LEZ sequencer**, the echo round-trip
  works, and all **22 skills** are present.
- All phases 0–5 implemented and integration-verified.
- Real **ECIES + AES-256-GCM** crypto; **signed agent cards**; **autonomous A2A pay loop**;
  **runtime plugin skill loader** (off by default, fail-closed, cannot shadow a built-in);
  **pluggable LLM** (Anthropic + OpenAI + NoOp fallback, no bundled model).
- **Real-proof demo scripts committed** (`demo-realproof.sh`, `run-sequencer-realproof.sh`,
  `RISC0_DEV_MODE=0`).

### Skills (22)
Wallet (3) · Storage (4) · Messaging (3) · A2A coordination (6) · Blockchain/program (3) · Meta (3).

### Why CI is the source of truth, not local
A local build OOM-crashes the WSL VM (`wallet-ffi` + RISC0 exceed available memory, no local
Cachix/RISC0 substituter). CI — with the Cachix cache `logos-pilot-johnniedom` and a disk-freed
runner — is where green is confirmed.

---

## What's done

| Phase | Status |
|-------|--------|
| Phase 0: Infrastructure | DONE — builds, loads modules, CI-green |
| Phase A: LLM Provider | DONE — Anthropic + OpenAI + NoOp fallback |
| Phase B: Real Encryption | DONE — AES-256-GCM + ECIES secp256k1 |
| Phase C: pilot CLI | DONE — deploy, chat, verify, discover, configure, status |
| Phase D: Skill Interface | DONE — 22 skills, PilotSkill trait, SkillRegistry |
| Phase E: Integration | DONE — verified against real modules |
| Phase 1: Identity + Wallet | DONE — wallet-ffi keys (`KeyChain::new_os_random`), persistence |
| Phase 2: Owner Channel | DONE — ECIES via delivery_module, Waku relay |
| Phase 3: Spending FSM | DONE — 9-state machine, SQLite WAL persistence |
| Phase 4: Storage + Messaging | DONE — upload/share/send |
| Phase 5: A2A Transport | DONE — signed cards, inbound task server, discovery, autonomous pay loop |
| CLI Interactive | DONE — daemon mode, REPL, stale-state recovery |
| CI pipeline | DONE — build + unit (109) + E2E all green |

---

## What's left

Full, unsoftened list in [`KNOWN_LIMITATIONS.md`](../KNOWN_LIMITATIONS.md). Summary:

### Blocked on external infrastructure (not code)
| Item | Why |
|------|-----|
| F9 — ≥3 use cases demoed end-to-end on testnet | No public LEZ testnet endpoint exists yet |
| F10 — ≥5 third-party deployments | Same — no public network for third parties to deploy against |
| Public testnet deployment | Same — demos run on local Docker devnet / sequencer |

### Upstream platform gap (not a Pilot defect)
| Item | Why |
|------|-----|
| `program.query` / `program.call` / `program.deploy` | No program-op method exists on the wallet module / `wallet-ffi` at the pinned revisions (`logos-execution-zone-module @ 5d42559`, `lssa @ cf3639d`); deploy needs a direct `NSSATransaction` the module doesn't surface. The three skills are fully wired through the spend/owner/trust pipeline and return an honest `unsupported (verified)`; they activate when upstream lands the API, with no Pilot-side redesign. |

### Pilot's own remaining scope (KNOWN_LIMITATIONS §2–5)
| Item | What |
|------|------|
| A2A: no owner-execute for parked risky inbound tasks | Risky stranger tasks correctly park at `input-required`; there is no owner command to finish them yet (inbound `wallet-send` is the exception — it's resumable via its spend request) |
| A2A: `agent.ask` runs the LLM synchronously in the delivery loop | Correct, but serializes other inbound A2A handling while it runs |
| A2A: TOFU first-seen squatting | Pinning protects against a later key-swap on a pinned npk, not a first-seen forgery |
| A2A: autonomous pay needs prior card discovery | Otherwise settles `accepted-nopay` until the doer's card is discovered |
| Real on-chain settlement + live 2-agent A2A | CI-green for build/unit/E2E, but real funding/spending with RISC0 proofs and the live two-agent round trip are still the manual / video path, not automated |
| Demo video | Not yet recorded/committed (storyboard exists in `private/`) |

> A deep adversarial A2A review is the active QA pass on the items above — findings feed back into this list.

---

## Active thread — sovereign funding (`feat/sovereign-funding`)
Self/owner funding flow under **real proofs** (`RISC0_DEV_MODE=0`) against a sequencer.

---

## Phase order (unchanged — do not reorder)
0: Infrastructure → 1: Identity + Wallet → 2: Owner Channel → 3: Spending FSM →
4: Storage + Messaging → 5: A2A Transport → 6: Integration Tests → 7: Basecamp UI → 8: Ship.

---

## Appendix — historical record

### Key architectural decisions (made during 2026-05 integration)
- **Owner channel uses `delivery_module`** (one layer below `chat_module`) — the Qt-wrapped Waku
  transport with module-to-module dispatch; the owner channel uses it directly + its own ECIES for
  self-contained control (see `docs/owner-channel.md`).
- **Agent generates its own ECIES keypair**, separate from wallet keys (wallet-ffi only exposes
  public keys).
- **Storage configured with `{"nat":"none"}`** — disables UPnP for WSL compatibility.
- **Waku `cluster-id=2`, 8 shards** — matches the Logos delivery network configuration.
- **`LogosResult` extraction** — storage module returns a custom Qt type, extracted via
  `result.value<LogosResult>().value`.

### Bugs found and fixed during integration (institutional memory)
| # | Bug | Fix |
|---|-----|-----|
| 1 | `logosAPI_` always NULL | Inject `onInit(LogosAPI*)` override via sed in flake.nix |
| 2 | capability_module stub crashes | Install real `.so` from nix store |
| 3 | SQLite dir not created | `mkdir()` before `sqlite3_open()` |
| 4 | Wallet camelCase methods | `getBalance` → `get_balance`, `transferPrivate` → `transfer_private` |
| 5 | Wallet config missing field | Added `"initial_accounts": []` |
| 6 | Wallet amount format | Plain int → 32-char LE hex string |
| 7 | Spending FSM false success | Check result string for "fail"/"error" |
| 8 | owner-channel transport choice | delivery_module + ECIES (one layer below chat) for all messaging |
| 9 | Storage NAT timeout | `{"nat":"none"}` config |
| 10 | Storage LogosResult extraction | `result.value<LogosResult>().value` |
| 11 | Storage download method name | `downloadFile` → `downloadChunks` |
| 12 | Owner channel plaintext | ECIES encryption with owner public key |
| 13 | Owner channel one-way | Added incoming message event listener + decrypt + processOwnerMessage |
| 14 | Waku topic 5 segments | Changed to 4-segment format |
| 15 | Waku missing cluster/shards | Added `clusterId:2, shards:[0..7]` |
| 16 | Waku peer port wrong | Container port 60000 → host port 30303 |
| 17 | CLI daemon stale state | Detect dead PID, clean state, restart fresh |
