# Pilot — Autonomous AI Agent Module for Logos

**LP-0008** | Logos Lambda Prize Submission

A Logos Core module that runs an autonomous AI agent with native access to the full Logos stack: a shielded LEZ wallet, encrypted Logos Storage, end-to-end encrypted messaging, and A2A agent coordination — all within spending limits set by its owner.

## Documentation

- [Architecture](docs/architecture.md) · [Security Model](docs/security-model.md) · [Agent-to-Agent (A2A) Protocol](docs/agent-to-agent.md) · [Payment Model](docs/payment-model.md) · [Owner Channel](docs/owner-channel.md) · [Skill Interface](docs/skill-interface.md)
- Guides: [Owner Guide](docs/owner-guide.md) (chatting, contacts, approvals) · [Deployment Guide](docs/deployment-guide.md) · [Developer Guide](docs/DEVELOPER_GUIDE.md) · **[Troubleshooting](docs/troubleshooting.md)** (symptom-indexed field fixes)
- **[Known Limitations](KNOWN_LIMITATIONS.md)** — the complete, honest list of what Pilot does not (yet) do, with upstream-gap evidence and CI/testnet status. Read this first.

## Quick Start

Prerequisites: Nix with flakes enabled, Docker installed.

### Step 1: Build everything

```bash
# Build the C++ module + unit tests
cd pilot-module
nix build --extra-experimental-features 'nix-command flakes'
nix build .#unit-tests --extra-experimental-features 'nix-command flakes' -L
nix build .#lgx --extra-experimental-features 'nix-command flakes' -o result-lgx

# Build the CLI
cd ../pilot-cli
nix build --extra-experimental-features 'nix-command flakes'
cd ..
```

### Step 2: Start infrastructure

```bash
# Terminal 1: Start the LEZ sequencer
./run-sequencer.sh

# Terminal 2: Start the Waku node
docker-compose up -d
```

### Step 3: Install modules

```bash
# Installs pilot + all dependency modules from nix cache
./setup-modules.sh
```

### Step 4: Deploy and chat

```bash
# Deploy: creates identity, selects LLM, publishes Agent Card (local sequencer by default)
./pilot-cli/result/bin/pilot deploy

# Same, against the public LEZ testnet (points the wallet at https://testnet.lez.logos.co)
./pilot-cli/result/bin/pilot deploy --testnet

# Headless deploy (no terminal: CI, a remote box, a systemd unit) — name provider + model in the env
PILOT_LLM_PROVIDER=anthropic ANTHROPIC_API_KEY=sk-... PILOT_LLM_MODEL=claude-sonnet-4-6-20250514 \
  PILOT_OWNER_NPK=<your npk> ./pilot-cli/result/bin/pilot deploy

# Chat: starts daemon, LLM-powered conversation
./pilot-cli/result/bin/pilot chat
```

### Step 5: Test suites (unit suite in CI; the E2E job runs demo.sh against the public testnet; two-agent suite runs locally)

```bash
# Unit tests — no runtime needed (run in CI on every push)
cd pilot-module && nix build .#unit-tests --extra-experimental-features 'nix-command flakes' -L && cd ..

# Single-agent integration (28) — needs sequencer
./test-phases.sh

# Two-agent Docker (30 checks) — needs sequencer + Docker
pkill -9 -f logos_host_qt; pkill -9 -f logoscore
rm -f ~/.cache/storage/dht/providers/LOCK
./test-two-agents-docker.sh
```

### Scripts reference

| Script | When to run | What it does |
|--------|------------|--------------|
| `run-sequencer.sh` | First | Boots the standalone LEZ sequencer in dev mode on **:3040** (the endpoint the wallet reads), **fresh genesis each start — an already-funded wallet goes stale with the wiped chain and must be re-funded** (to keep a funded wallet, boot without the wipe: see [docs/troubleshooting.md](docs/troubleshooting.md)) — needs a local logos-execution-zone build (see the script header) |
| `docker-compose up -d` | First | Starts Waku node on port 30303 |
| `setup-modules.sh` | After build | Installs all modules from nix cache to `/tmp/pilot-logoscore/modules` |
| `demo.sh` | Anytime, from a clean clone | End-to-end against the public testnet: build → load → 23 skills → self-fund from the faucet → spend through the spending FSM (verified on chain) → vault round-trip; every step asserted, exit 1 on failure |
| `test-phases.sh` | After setup | 28 single-agent integration tests |
| `test-two-agents-docker.sh` | After setup | 30 two-agent cross-network checks incl. a paid task settled on chain |
| `install-basecamp.sh` | For GUI | Installs pilot into Logos Basecamp |
| `run-sequencer-realproof.sh` | For the spec demo | Boots the standalone LEZ sequencer with **real** RISC0 proofs (`RISC0_DEV_MODE=0`) — needs the prerequisites below |
| `demo-realproof.sh` | For the spec demo | End-to-end flow against the real-proof sequencer (the flow the demo video captures) |

### What needs what (you don't always need the full stack)

The CLI degrades gracefully — you only need the heavy infrastructure for the parts that
use it. Identity creation is local; `deploy` never hard-fails without a sequencer (it
shows a `0` balance, warns on card publish, and completes).

| You want to… | Sequencer? | Waku? | How |
|--------------|-----------|-------|-----|
| Run the unit tests | No | No | `cd pilot-module && nix build .#unit-tests -L` (zero infra) |
| `deploy` + `chat` + list skills | No | No | build → `setup-modules.sh` → `pilot deploy` / `pilot chat` (needs an LLM API key for chat) |
| Owner channel / messaging / A2A | No | **Yes** | also run `docker-compose up -d` |
| Balance / funding / send / spending | **Yes** | — | also run `./run-sequencer.sh` |

So the fastest way to see the agent run is **build → `setup-modules.sh` → `pilot deploy` → `pilot chat`** — no `run-sequencer.sh`, no `docker-compose`.

### Real-proof demo (`RISC0_DEV_MODE=0`)

The spec requires an end-to-end demo with **real** RISC0 proofs, shown on camera. Unlike the
public-rail `demo.sh` (which clone-and-runs against the public testnet with no client proof), the real-proof
flow needs a standalone LEZ sequencer + the RISC0 toolchain — a documented prerequisite, since
the sequencer is a separate project not bundled in this repo.

**Prerequisites (one-time):**
1. Build the standalone LEZ sequencer from `logos-execution-zone` at the circuits-matched rev:
   `cargo build --release --features standalone -p sequencer_runner`.
2. Install the RISC0 toolchain so `r0vm` is on PATH: `rzup install`.
3. ~16 GB RAM (real proving is heavy; lower `RISC0_SEGMENT_PO2` to trade RAM for time).

**Run it:**

```bash
# Terminal 1 — real-proof sequencer on :3040 (point LEZ at your execution-zone checkout)
LEZ=/path/to/logos-execution-zone ./run-sequencer-realproof.sh

# Terminal 2 — the end-to-end demo (build → deploy → self-fund → balance → use cases)
export ANTHROPIC_API_KEY=sk-...        # or another provider, for chat / agent.ask
./demo-realproof.sh
```

Each on-chain step (funding, an above-threshold transfer) triggers visible `r0vm` proof
generation — that on-screen output is what confirms `RISC0_DEV_MODE=0` was active. The recorded
video of this run is the submission's proof-of-real-proving artifact.

## What It Does

The Pilot agent is a first-class participant in the Logos ecosystem. It can:

- **Hold and spend tokens** in its own shielded LEZ wallet
- **Store and retrieve encrypted files** via Logos Storage
- **Communicate with its owner** via end-to-end encrypted Logos Messaging
- **Discover and coordinate with other agents** using the A2A protocol
- **Spend tokens on-chain** autonomously within owner-set limits (program execution is a verified upstream gap)

The owner deploys the agent with a single CLI command and interacts with it from any Logos app instance — no server configuration, no exposed APIs, no custodian.

## Architecture

```
pilot-module/
├── src/
│   ├── pilot_impl.h          # Public API — 23 skills, pure C++ types
│   ├── pilot_impl.cpp         # Core: init, database, echo
│   ├── pilot_identity.cpp     # Identity + wallet (Phase 1)
│   ├── pilot_owner.cpp        # Owner channel (Phase 2)
│   ├── pilot_spending.cpp     # 9-state spending FSM (Phase 3)
│   ├── pilot_storage.cpp      # Encrypted storage (Phase 4)
│   ├── pilot_messaging.cpp    # Messaging + groups (Phase 4)
│   ├── pilot_meta.cpp         # Skills list, status, config (Phase 4)
│   └── pilot_a2a.cpp          # A2A protocol + blockchain (Phase 5)
├── metadata.json              # Module config + dependencies
├── flake.nix                  # Nix build definition
└── CMakeLists.txt             # Build rules
```

## Skills (22 total)

### Wallet (3)
| Skill | Method | Description |
|-------|--------|-------------|
| wallet.balance | `walletBalance()` | Returns shielded token balance |
| wallet.send | `walletSend(recipient, amount, reason)` | Sends LEZ tokens (threshold-enforced) |
| wallet.history | `walletHistory()` | Transaction history from local ledger |

### Storage (4)
| Skill | Method | Description |
|-------|--------|-------------|
| storage.upload | `storageUpload(path, label)` | Encrypts and uploads a file |
| storage.download | `storageDownload(cid, path)` | Retrieves and decrypts a file |
| storage.list | `storageList()` | Lists all stored files |
| storage.share | `storageShare(cid, recipient)` | Sends the file key, sealed to the recipient's encryption key; the receiver records it so its own `storage.download` can fetch and decrypt the CID |

### Messaging (3)
| Skill | Method | Description |
|-------|--------|-------------|
| messaging.send | `messagingSend(recipient, message)` | Direct: sealed (ECIES) to the recipient's encryption key — a bare key, or the peer's card JSON. `recipient` = `group:<id>` sends to a joined group, sealed (AES-256-GCM) with the group key |
| messaging.join | `messagingJoin(groupId)` | Joins a group we were invited to (we hold its key); joined groups are polled |
| messaging.create_group | `messagingCreateGroup(membersJson)` | Creates a group with a fresh group key and sends each member (JSON array of keys or cards) an invite sealed to that member alone |

Receiving is a method, not a skill: `messagingInbox()` lists what arrived — direct and group
messages, group invites, file shares — newest first. Before 2026-09-04 every one of those was
decrypted and dropped on arrival; the sender saw `"sent": true`. An agent's inbox is named after
its encryption key (`_logos.enc_key` in its card) and is listened on only while the agent is
open for hire. `storagePeerInfo()` / `storageConnect(peerId, addrs)` let one agent's storage
node dial another's so a shared CID can be fetched over the storage network.

### Agent Coordination — A2A (6)
| Skill | Method | Description |
|-------|--------|-------------|
| agent.ask | `agentAsk(prompt)` | Answers a prompt with the agent's LLM (pure compute; price 5) — the only skill served autonomously to paying strangers |
| agent.card | `agentCard()` | Publishes A2A Agent Card to discovery topic |
| agent.discover | `agentDiscover(topic)` | Discovers peer agents via Waku Store |
| agent.task | `agentTask(address, skill, params)` | Sends a task request (JSON-RPC 2.0) |
| agent.subscribe | `agentSubscribe(address, taskId)` | Streams task status updates |
| agent.cancel | `agentCancel(address, taskId)` | Cancels a running task |

`agent.ask` is the only skill served autonomously to paying strangers — a pure-compute LLM query (no files, identity, or funds). All other inbound skills are owner-gated.

### Blockchain (3)
| Skill | Method | Description |
|-------|--------|-------------|
| program.query | `programQuery(programId, params)` | Reads state from a LEZ program — _verified upstream gap: no program-op method exists on the wallet module / wallet-ffi at the pinned revs; calls the real method and returns an honest `unsupported (verified)` error_ |
| program.call | `programCall(programId, instruction, params)` | Submits a transaction — _verified upstream gap (same as above); does not transact_ |
| program.deploy | `programDeploy(binaryPath)` | _Verified upstream gap: deploy on LEZ is a direct `NSSATransaction` the wallet module does not expose. Attempts the real call and returns an honest `unsupported (verified)` error — it does not route through owner approval._ |

> **Known limitation (verified upstream gap, not a Pilot defect):** the `program.*`
> skills are wired through the full agent pipeline, but a direct source audit of the
> pinned dependency revisions found **no program-operation method of any name** on
> the wallet module or `wallet-ffi`. Each skill calls the real underlying method and
> returns an honest `unsupported (verified)` error rather than faking success. See
> [KNOWN_LIMITATIONS.md §1](KNOWN_LIMITATIONS.md).

### Meta (3)
| Skill | Method | Description |
|-------|--------|-------------|
| meta.skills | `metaSkills()` | Lists all available skills |
| meta.status | `metaStatus()` | Agent status: balance, pending, channel state |
| meta.configure | `metaConfigure(key, value)` | Runtime configuration updates |

## Spending Threshold Mechanism

The agent enforces a 9-state finite state machine for every transaction:

```
CREATED ─┬─(within limits)──────────────► EXECUTING ─► COMPLETED
         │                                          └► TX_FAILED
         └─(over a limit)─► HELD ─► NOTIFIED ─┬─/approve ─► APPROVED ─► EXECUTING ─► COMPLETED
                                              │                                  └► TX_FAILED
                                              ├─/reject ──► REJECTED
                                              └─timeout ──► EXPIRED
```

(See [docs/security-model.md §3](docs/security-model.md) for the authoritative 9-state diagram.)

- **Below threshold**: Executes autonomously (CREATED → EXECUTING → COMPLETED)
- **Above threshold**: Holds and notifies the owner over the ECIES owner channel (delivery_module), waits for `/approve` or `/reject`
- **Ambiguity defaults to inaction** — never execution
- **Crash recovery**: Pending transactions are re-notified on restart
- **Persistence**: SQLite WAL mode, `PRAGMA synchronous=FULL`

## A2A Protocol Compatibility

Agent-to-agent coordination follows the [A2A specification](https://a2a-protocol.org/latest/specification/):

- **Agent Cards**: Published to Waku discovery topic with capabilities, skills, and pricing
- **Task lifecycle**: `tasks/send`, `tasks/sendSubscribe`, `tasks/cancel` (JSON-RPC 2.0)
- **Transport**: Waku relay with NATS-style reply topics
- **`_logos` extension**: NPK identity, Waku topics, LEZ payment, pricing per skill

## Security Model

| Action | Without Owner Approval | With Owner Approval |
|--------|----------------------|-------------------|
| Read balance | Yes | — |
| Send below threshold | Yes | — |
| Send above threshold | No | Yes |
| Upload/download files (owner-initiated) | Yes | — |
| Share files (owner-initiated) | Yes | — |
| Deploy programs | No | No — unsupported upstream (KNOWN_LIMITATIONS.md §1) |
| Send messages (owner-initiated) | Yes | — |
| Agent-to-agent tasks (owner-initiated) | Yes | — |
| Stranger-initiated risky A2A skill (storage/messaging/wallet/program) | No — owner-gated | Yes |

Only `agent.ask` (a pure-compute LLM query) is served autonomously to a stranger; every other inbound storage/messaging/wallet/program skill is owner-gated (`input-required`). See [docs/security-model.md §4/§6](docs/security-model.md).

- **Identity**: shielded LEZ account via `create_account_private` / `get_private_account_keys` on `lez_core`; plus a separate ECIES messaging/signing keypair
- **Encryption**: ECIES (secp256k1) for owner channel + agent inboxes + file sharing, AES-256-GCM for files
- **Key storage**: Wallet keys in LEZ wallet module, agent ECIES keypair in SQLite config

## Extensibility — Plugin Loader

Third parties can add skills **without recompiling the core** via a Qt plugin
(`PilotPluginInterface`). The loader is:

- **Off by default, fail-closed.** It loads nothing unless the operator sets
  `PILOT_ENABLE_PLUGINS` to `1`/`true`/`yes`/`on`.
- **Loaded from an operator-trusted directory** — `~/.pilot/plugins` (override with
  `PILOT_PLUGINS_DIR`). Placing a file there is an explicit act of operator trust.
- **Explicitly NOT a sandbox.** A loaded plugin runs in-process with **full agent
  privileges** (it can reach the agent's keys and funds). No isolation is claimed.

See [docs/security-model.md §5](docs/security-model.md) for the trust model and
[examples/skill-weather/](examples/skill-weather/) for a worked example.

## Dependencies

| Module | Purpose |
|--------|---------|
| `lez_core` | Shielded wallet, account creation, transfers |
| `delivery_module` | Waku pub/sub messaging transport (also carries the E2E owner channel) |
| `storage_module` | Encrypted file upload/download |

## Build Targets

All commands run from `pilot-module/`. Nix flakes must be enabled.

```bash
nix --extra-experimental-features "nix-command flakes" build          # Module binary (.so)
nix --extra-experimental-features "nix-command flakes" build .#lgx    # Installable package (.lgx)
nix --extra-experimental-features "nix-command flakes" build .#install       # Module + manifest for logoscore
nix --extra-experimental-features "nix-command flakes" build .#unit-tests -L # Run unit tests (188)
nix --extra-experimental-features "nix-command flakes" build .#include       # Module headers (for dependents)
```

Note: `result/` is a symlink that changes with each build. It only holds the last target you built.

## Verifying the Module

### 1. Module inspector (`lm`)

After `nix build`, verify the module loads and all methods are registered:

```bash
lm result/lib/pilot_plugin.so
```

Expected output:
```
Plugin Metadata:
================
Name:         pilot
Version:      1.0.0
Description:  Autonomous AI agent with wallet, storage, and messaging on LEZ
Author:       Johnnie Dom
Type:         core
Dependencies: lez_core, delivery_module, storage_module

Plugin Methods:
===============
QString echo(QString input)
bool initialize(QString dataDir)
QString walletBalance()
QString walletSend(QString recipient, int amount, QString reason)
... (33 methods total)
```

### 2. Unit tests

```bash
nix --extra-experimental-features "nix-command flakes" build .#unit-tests -L
```

Builds the 44-test unit suite covering all phases — echo, crypto round-trips, skill registry, LLM factory, owner message routing. The unit tests need no runtime. Results pending CI verification — see [KNOWN_LIMITATIONS.md §4](KNOWN_LIMITATIONS.md).

### 3. Loading into logoscore

logoscore finds modules in a `modules/` directory next to its `bin/` directory. To test:

```bash
# Set up the directory structure
mkdir -p /tmp/logos-test/bin /tmp/logos-test/modules

# Copy logoscore binary (find it in your nix store)
cp $(find /nix/store -name ".logoscore-wrapped" -path "*liblogos-build*" | head -1) /tmp/logos-test/bin/logoscore

# Copy the module
nix --extra-experimental-features "nix-command flakes" build
cp result/lib/pilot_plugin.so /tmp/logos-test/modules/

# Set Qt environment and run
export QT_PLUGIN_PATH=$(find /nix/store -maxdepth 1 -name "*-qtbase-6.*" -not -name "*only*" -not -name "*dev*" -type d | head -1)/lib/qt-6/plugins
export LD_LIBRARY_PATH=$(find /nix/store -maxdepth 1 -name "*-logos-liblogos-build-*" -type d | head -1)/lib
/tmp/logos-test/bin/logoscore
```

Expected: logoscore finds and loads the module:
```
Found plugin: "/tmp/logos-test/modules/pilot_plugin.so"
Name: "pilot", Version: "1.0.0", Author: "Johnnie Dom"
Successfully processed plugin: "pilot"
```

Note: Calling methods (e.g. `logoscore call pilot echo hello`) requires the dependency modules (wallet, delivery, storage) to be running. See Installation below.

## Installation

### Prerequisites

The pilot module depends on 3 other Logos modules. These must be installed in Logos Basecamp first:

1. `lez_core` — Shielded wallet and account creation
2. `delivery_module` — Waku pub/sub messaging (and the E2E-encrypted owner channel)
3. `storage_module` — Encrypted file storage

Install these from the Logos module registry via Basecamp's package manager.

### Install Pilot Module

#### Option A: From .lgx package (recommended)
```bash
cd pilot-module
nix --extra-experimental-features "nix-command flakes" build .#lgx
lgpm install --file result/logos-pilot-module-lib.lgx --allow-unsigned
```

#### Option B: Manual copy
```bash
cd pilot-module
nix --extra-experimental-features "nix-command flakes" build .#install
cp -r result/modules/pilot /path/to/logoscore/modules/
```

### Using the Agent

Once installed alongside its dependencies:

```bash
# Deploy the agent (creates identity, wallet, owner channel)
logoscore call pilot initialize /path/to/data

# Check agent status
logoscore call pilot metaStatus

# List all skills
logoscore call pilot metaSkills

# Check wallet balance
logoscore call pilot walletBalance

# Send tokens (below threshold = auto, above = needs approval).
# The recipient is the payee's FULL keys JSON — a bare hex account id only
# works for accounts this wallet owns (docs/troubleshooting.md "TX_FAILED").
logoscore call pilot walletSend '{"nullifier_public_key":"<npk-hex>","viewing_public_key":"<vpk-hex>"}' 50 "payment for services"

# Upload a file to encrypted storage
logoscore call pilot storageUpload /path/to/file.txt "my document"

# Send a message to another agent: the recipient is the peer's ENCRYPTION key (its card's
# _logos.enc_key) or the card JSON itself; "group:<id>" sends to a joined group
logoscore call pilot messagingSend <peer-enc-key-hex> "hello from pilot"
# Read what arrived (direct + group messages, invites, file shares); agentPoll pulls the relay store first
logoscore call pilot agentPoll
logoscore call pilot messagingInbox

# Publish Agent Card for A2A discovery
logoscore call pilot agentCard

# Send a task to another agent
logoscore call pilot agentTask targetNpk wallet-balance "{}"
```

## CLI (Pilot Chat)

The Nim-based CLI provides deploy, chat, and management commands:

```bash
# Build the CLI
cd pilot-cli && nix build --extra-experimental-features 'nix-command flakes'

# Deploy an agent (creates identity, configures LLM, publishes Agent Card)
./result/bin/pilot deploy

# Chat with your agent (starts daemon, LLM-powered conversation)
./result/bin/pilot chat

# Check agent status
./result/bin/pilot status
```

Deploy walks you through:
1. Agent identity creation on the sequencer
2. LLM provider selection (Anthropic, OpenAI, DeepSeek, Google, OpenRouter, Groq)
3. Model selection per provider
4. Owner key binding
5. Agent Card publication

Chat features:
- Conversation memory (20 turns)
- Slash commands (`/balance`, `/send`, `/approve`, `/reject`, `/pending`, `/upload`, `/download`, `/files`, `/skills`, `/status`, `/discover`, `/help`)
- **@contacts** — save a payee's keys JSON once as `~/.pilot/contacts/<name>.json`, then `/send @name 20 reason` or plain "send 20 LEZ to @name" both resolve it (see [Owner Guide](docs/owner-guide.md))
- Interactive approval prompt when a send exceeds the spending limit (Approve / Reject / Skip)
- Natural language → action dispatch via LLM — with a guardrail: lines containing raw wallet-key material are never passed through the LLM (retyped keys arrive corrupted)
- Formatted output for skills, balance, status
- Animated spinner during LLM calls

## Testing (86 total — results pending CI verification, KNOWN_LIMITATIONS.md §4)

### Prerequisites

```bash
# Terminal 1: Start the sequencer
./run-sequencer.sh

# Terminal 2: Start the Waku node
docker-compose up -d
```

### Suite 1: Unit Tests (188 tests)

Tests crypto, skill registry, LLM factory, and core module behavior. No runtime needed.

```bash
cd pilot-module
nix build .#unit-tests --extra-experimental-features 'nix-command flakes' -L
# 44-test suite; results pending CI verification (KNOWN_LIMITATIONS.md §4)
```

### Suite 2: Single-Agent Integration (28 tests)

Tests all 28 methods across phases 1-5 against a live sequencer.

```bash
# Requires: sequencer running on port 3040 (./run-sequencer.sh), modules installed
./setup-modules.sh   # one-time: installs all modules from nix cache
./test-phases.sh     # runs all 28 tests
```

Tests cover:
- Phase 1: Identity + Wallet (initialize, NPK, account, balance, history)
- Phase 2: Owner Channel (establish, get ID, send)
- Phase 3: Spending FSM (limits, create/approve/reject, send)
- Phase 4: Storage (upload, list, download, share)
- Phase 4: Messaging (send, join, create group)
- Phase 5: A2A (card, discover, task, subscribe, cancel)
- Meta: skills, status, configure

### Suite 3: Two-Agent Integration (30 checks)

Tests cross-agent communication with Agent A on host and Agent B in Docker.

```bash
# Requires: sequencer + Waku node running, Docker available
./test-two-agents-docker.sh   # prints "Results: N passed, M failed", exits non-zero on any failure
```

Checks cover:
- Both agents create unique identities on the sequencer
- Each agent is opened for hire, then asserted to be listening on the inbox its own
  Agent Card advertises (the check that catches an unhireable agent)
- Agent A publishes its Agent Card and **Agent B discovers it** over the broadcast
  discovery topic (broken until 2026-07-28, see
  [`KNOWN_LIMITATIONS.md`](KNOWN_LIMITATIONS.md) §7 for what was wrong). A peer can
  also be imported out-of-band: `pilot peer add <card.json>`
- Bidirectional encrypted messaging (A→B and B→A)
- Storage upload + encrypted file key sharing
- Full A2A task lifecycle (send task, subscribe, cancel)
- Cross-agent wallet transfer, and a **paid task** whose settlement is asserted on the
  chain: the payer's balance must drop by exactly the declared price within four blocks

The suite reports failures rather than skipping them. Last full run on the development
box: 2026-08-28, 30 of 30 passed (direct transfer 100 → 99, paid task 99 → 94, both
read back from the chain). It runs against a local dev-mode sequencer, not in CI.

Docker setup (no installation required):
- Agent B runs in `ubuntu:22.04` container with `/nix/store` mounted read-only
- Isolated network namespace eliminates port conflicts
- `host.docker.internal` bridges to host sequencer and Waku node

### Running All Suites

```bash
# 1. Unit tests
cd pilot-module && nix build .#unit-tests --extra-experimental-features 'nix-command flakes' -L

# 2. Single-agent (sequencer must be running)
cd .. && ./test-phases.sh

# 3. Two-agent Docker (sequencer + Waku must be running)
pkill -9 -f logos_host_qt; pkill -9 -f logoscore   # clean up suite 2
rm -f ~/.cache/storage/dht/providers/LOCK
./test-two-agents-docker.sh
```

Suite sizes (results pending CI verification — KNOWN_LIMITATIONS.md §4):

| Suite | Tests |
|-------|-------|
| Unit tests | 44 |
| Single-agent | 28 |
| Two-agent Docker | 14 |
| **Total** | **86** |

## Known Limitations

> The full, honest list — upstream program-op gap (with pinned-revision evidence),
> A2A residual minors, the plugin-loader trust caveats, pending-CI build/test status,
> and the testnet-evidence + demo-video gaps — is in
> **[KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md)**. The items below are a summary.

- **Storage download is async** — `downloadChunks` returns a session ID; data arrives via events. Synchronous callers get a "downloading" status.
- **Two storage modules can't coexist** — global LevelDB cache at `~/.cache/storage/`. Multi-agent needs separate machines or Docker.
- **NAT detection in WSL** — delivery module spends 12s probing for UPnP/NAT-PMP gateways that don't exist. Set `PILOT_NAT=extip:127.0.0.1` to skip.
- **Qt RO Timeout doesn't cancel FFI** — `Timeout(15000)` returns null to caller but the remote C call keeps running.
- `programCall`/`programQuery`/`programDeploy` — verified upstream gap: no program-op method of any name exists on the wallet module / wallet-ffi at the pinned revs; the skills call the real method and return an honest `unsupported (verified)` error (KNOWN_LIMITATIONS.md §1)
- RISC0 proof generation requires `RISC0_DEV_MODE=0` only when a SPEL guest binary exists

## License

Licensed under either of **MIT** ([LICENSE](LICENSE)) or **Apache-2.0**
([LICENSE-APACHE](LICENSE-APACHE)) at your option.
