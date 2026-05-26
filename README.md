# Pilot — Autonomous AI Agent Module for Logos

**LP-0008** | Logos Lambda Prize Submission

A Logos Core module that runs an autonomous AI agent with native access to the full Logos stack: a shielded LEZ wallet, encrypted Logos Storage, end-to-end encrypted messaging, and A2A agent coordination — all within spending limits set by its owner.

## Quick Start

```bash
# Prerequisites: Nix with flakes enabled
cd pilot-module

# 1. Build the module
nix --extra-experimental-features "nix-command flakes" build

# 2. Verify the module loads (shows all 36 methods)
lm result/lib/pilot_plugin.so

# 3. Run unit tests (44/44)
nix --extra-experimental-features "nix-command flakes" build .#unit-tests -L

# 4. Build the installable .lgx package
nix --extra-experimental-features "nix-command flakes" build .#lgx
# Output: result/logos-pilot-module-lib.lgx

# 5. Install into Logos Basecamp
lgpm install --file result/logos-pilot-module-lib.lgx --allow-unsigned
```

Or run the automated demo:

```bash
cd ..
./demo.sh
```

## What It Does

The Pilot agent is a first-class participant in the Logos ecosystem. It can:

- **Hold and spend tokens** in its own shielded LEZ wallet
- **Store and retrieve encrypted files** via Logos Storage
- **Communicate with its owner** via end-to-end encrypted Logos Messaging
- **Discover and coordinate with other agents** using the A2A protocol
- **Execute on-chain actions** autonomously within owner-set spending limits

The owner deploys the agent with a single CLI command and interacts with it from any Logos app instance — no server configuration, no exposed APIs, no custodian.

## Architecture

```
pilot-module/
├── src/
│   ├── pilot_impl.h          # Public API — 21 skills, pure C++ types
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

## Skills (21 total)

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
| storage.share | `storageShare(cid, recipientNpk)` | Shares file access with another identity |

### Messaging (3)
| Skill | Method | Description |
|-------|--------|-------------|
| messaging.send | `messagingSend(recipient, message)` | Sends encrypted message to an inbox |
| messaging.join | `messagingJoin(groupId)` | Joins a group topic |
| messaging.create_group | `messagingCreateGroup(membersJson)` | Creates a group and invites members |

### Agent Coordination — A2A (5)
| Skill | Method | Description |
|-------|--------|-------------|
| agent.card | `agentCard()` | Publishes A2A Agent Card to discovery topic |
| agent.discover | `agentDiscover(topic)` | Discovers peer agents via Waku Store |
| agent.task | `agentTask(address, skill, params)` | Sends a task request (JSON-RPC 2.0) |
| agent.subscribe | `agentSubscribe(address, taskId)` | Streams task status updates |
| agent.cancel | `agentCancel(address, taskId)` | Cancels a running task |

### Blockchain (3)
| Skill | Method | Description |
|-------|--------|-------------|
| program.query | `programQuery(programId, params)` | Reads state from a LEZ program |
| program.call | `programCall(programId, instruction, params)` | Submits a transaction (threshold-enforced) |
| program.deploy | `programDeploy(binaryPath)` | Deploys a program binary (requires approval) |

### Meta (3)
| Skill | Method | Description |
|-------|--------|-------------|
| meta.skills | `metaSkills()` | Lists all available skills |
| meta.status | `metaStatus()` | Agent status: balance, pending, channel state |
| meta.configure | `metaConfigure(key, value)` | Runtime configuration updates |

## Spending Threshold Mechanism

The agent enforces a 9-state finite state machine for every transaction:

```
CREATED → HELD → NOTIFIED → APPROVED → EXECUTING → COMPLETED
                           → REJECTED
                                       → TX_FAILED
                                                   → EXPIRED
```

- **Below threshold**: Executes autonomously (CREATED → EXECUTING → COMPLETED)
- **Above threshold**: Holds and notifies owner via chat, waits for `/approve` or `/reject`
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
| Upload/download files | Yes | — |
| Share files | Yes | — |
| Deploy programs | No | Yes |
| Send messages | Yes | — |
| Agent-to-agent tasks | Yes | — |

- **Identity**: `KeyChain::new_os_random()` via wallet-ffi (not custom key generation)
- **Encryption**: ECIES (secp256k1) for owner channel + agent inboxes + file sharing, AES-256-GCM for files
- **Key storage**: Wallet keys in LEZ wallet module, agent ECIES keypair in SQLite config

## Dependencies

| Module | Purpose |
|--------|---------|
| `lez_wallet_module` | Shielded wallet, account creation, transfers |
| `delivery_module` | Waku pub/sub messaging transport |
| `storage_module` | Encrypted file upload/download |
| `chat_module` | Owner channel (E2E encrypted) |

## Build Targets

All commands run from `pilot-module/`. Nix flakes must be enabled.

```bash
nix --extra-experimental-features "nix-command flakes" build          # Module binary (.so)
nix --extra-experimental-features "nix-command flakes" build .#lgx    # Installable package (.lgx)
nix --extra-experimental-features "nix-command flakes" build .#install       # Module + manifest for logoscore
nix --extra-experimental-features "nix-command flakes" build .#unit-tests -L # Run unit tests (44/44)
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
Dependencies: lez_wallet_module, delivery_module, storage_module, chat_module

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

Runs 44 tests covering all phases — echo, crypto round-trips, skill registry, LLM factory, owner message routing. All tests pass in under 100ms.

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

Note: Calling methods (e.g. `logoscore call pilot echo hello`) requires the dependency modules (wallet, delivery, storage, chat) to be running. See Installation below.

## Installation

### Prerequisites

The pilot module depends on 4 other Logos modules. These must be installed in Logos Basecamp first:

1. `lez_wallet_module` — Shielded wallet and account creation
2. `delivery_module` — Waku pub/sub messaging
3. `storage_module` — Encrypted file storage
4. `chat_module` — E2E encrypted owner communication

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

# Send tokens (below threshold = auto, above = needs approval)
logoscore call pilot walletSend recipientNpk 50 "payment for services"

# Upload a file to encrypted storage
logoscore call pilot storageUpload /path/to/file.txt "my document"

# Send a message to another agent
logoscore call pilot messagingSend recipientNpk "hello from pilot"

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
- Slash commands (`/balance`, `/send`, `/skills`, `/status`, `/discover`, `/help`)
- Natural language → action dispatch via LLM
- Formatted output for skills, balance, status
- Animated spinner during LLM calls

## Testing (86/86)

### Prerequisites

```bash
# Terminal 1: Start the sequencer
./run-sequencer.sh

# Terminal 2: Start the Waku node
docker-compose up -d
```

### Suite 1: Unit Tests (44 tests)

Tests crypto, skill registry, LLM factory, and core module behavior. No runtime needed.

```bash
cd pilot-module
nix build .#unit-tests --extra-experimental-features 'nix-command flakes' -L
# Expected: 44 passed (< 100ms)
```

### Suite 2: Single-Agent Integration (28 tests)

Tests all 28 methods across phases 1-5 against a live sequencer.

```bash
# Requires: sequencer running on port 8080, modules installed
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

### Suite 3: Two-Agent Integration (14 tests)

Tests cross-agent communication with Agent A on host and Agent B in Docker.

```bash
# Requires: sequencer + Waku node running, Docker available
./test-two-agents-docker.sh   # runs all 14 tests
```

Tests cover:
- Both agents create unique identities on the sequencer
- Agent A publishes Agent Card, Agent B discovers it via Waku store
- Bidirectional encrypted messaging (A→B and B→A)
- Storage upload + encrypted file key sharing
- Full A2A task lifecycle (send task, subscribe, cancel)
- Cross-agent wallet transfer

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

| Suite | Tests | Time |
|-------|-------|------|
| Unit tests | 44 | ~8 min (build) + <1s (run) |
| Single-agent | 28 | ~3 min |
| Two-agent Docker | 14 | ~3 min |
| **Total** | **86** | **~14 min** |

## Known Limitations

- **Storage download is async** — `downloadChunks` returns a session ID; data arrives via events. Synchronous callers get a "downloading" status.
- **Two storage modules can't coexist** — global LevelDB cache at `~/.cache/storage/`. Multi-agent needs separate machines or Docker.
- **NAT detection in WSL** — delivery module spends 12s probing for UPnP/NAT-PMP gateways that don't exist. Set `PILOT_NAT=extip:127.0.0.1` to skip.
- **Qt RO Timeout doesn't cancel FFI** — `Timeout(15000)` returns null to caller but the remote C call keeps running.
- `programCall`/`programQuery` — wallet-ffi v0.1 does not expose program methods (LEZ v0.2.0-rc3 feature)
- RISC0 proof generation requires `RISC0_DEV_MODE=0` only when a SPEL guest binary exists

## License

MIT OR Apache-2.0
