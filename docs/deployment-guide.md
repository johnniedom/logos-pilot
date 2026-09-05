# Pilot Agent — Deployment Guide

## Prerequisites

- Linux (x86_64) or WSL2
- [Nix](https://nixos.org/download.html) with flakes enabled
- Docker (for sequencer + Waku node)
- An LLM API key (optional — agent works without it in command-only mode)

## Step 1: Start Infrastructure

```bash
# Terminal 1: Start the LEZ sequencer
./run-sequencer.sh

# Terminal 2: Start the Waku node
docker-compose up -d
```

Verify both are running:
```bash
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:3040/   # sequencer — any HTTP code = alive; 000 = down
docker ps                              # should show pilot-nwaku + sequencer
```

## Step 2: Build

```bash
# Build the C++ module
cd pilot-module
git init && git add -A
nix build --extra-experimental-features 'nix-command flakes'

# Run unit tests (208)
nix build .#unit-tests --extra-experimental-features 'nix-command flakes' -L

# Build LGX package
nix build .#lgx --extra-experimental-features 'nix-command flakes' -o result-lgx

# Build the CLI
cd ../pilot-cli
git init && git add -A
nix build --extra-experimental-features 'nix-command flakes'
cd ..
```

## Step 3: Install Modules

`pilot deploy` does this itself: any of the four modules the agent needs (`lez_core`,
`delivery_module`, `storage_module`, `pilot`) that is missing from the modules directory is installed
from the LGX packages the build left in the nix store before the daemon starts, and the deploy says
which ones it installed. The script is still there for installing everything by hand (including the
optional `chat_module` and `capability_module`):

```bash
./setup-modules.sh
```

Both install into `$PILOT_MODULE_PATH` (default `~/.pilot/modules`).

## Step 4: Deploy

```bash
./pilot-cli/result/bin/pilot deploy
```

Headless (no terminal: CI, a remote box, a systemd unit) — the two arrow selectors are skipped when
`PILOT_LLM_PROVIDER` (and optionally `PILOT_LLM_MODEL`) are set; the API key comes from the provider's
env var and the owner key from `PILOT_OWNER_NPK`, so the whole deploy needs no keystrokes:

```bash
PILOT_LLM_PROVIDER=anthropic ANTHROPIC_API_KEY=sk-... PILOT_LLM_MODEL=claude-sonnet-4-6-20250514 \
  PILOT_OWNER_NPK=<npk> ./pilot-cli/result/bin/pilot deploy
```

The wizard walks you through:

1. **Agent Identity** — creates a shielded wallet account on the sequencer, generates NPK/ISK keypair
2. **LLM Provider** — arrow-key selector: Anthropic, OpenAI, DeepSeek, Google Gemini, OpenRouter, Groq, or skip
3. **Model Selection** — 2-3 models per provider (e.g., claude-sonnet-4-6, gpt-4.1, deepseek-v4-pro)
4. **API Key** — enter your key (stored in SQLite, restored on restart)
5. **Owner Identity** — your secp256k1 public key (optional — skip for testing)
6. **Agent Card** — published to the Waku discovery topic

## Step 5: Chat

```bash
./pilot-cli/result/bin/pilot chat
```

First time asks your name (stored permanently). Then:

```
Pilot Chat
──────────────────────────────────────────────────
✓ Agent online
  Account       91996446eb22...
  Welcome back, Johnnie
  Type /help for commands, or just chat.

> hello
  pilot │ Hey Johnnie, ready to help. What do you need?

> /upload /mnt/c/Users/johnn/Desktop/doc.pdf my-doc
  pilot │   Uploaded  my-doc
        │   CID  zDvZRwzm3EEJDkF2Em...
        │   Encrypted  yes

> /files
  pilot │   Stored Files
        │   my-doc
        │     CID  zDvZRwzm3EEJDkF2Em...

> /download my-doc /tmp/doc.pdf
  pilot │   Downloaded  /tmp/doc.pdf
        │   Decrypted  yes

> /balance
  pilot │   Account  91996446eb22...
        │   Balance  1000 LEZ

> what can you do?
  pilot │ I can manage your wallet, encrypt and store files, send messages,
        │ discover other agents, and execute on-chain transactions. 23 skills total.
```

### Slash Commands

| Command | Description |
|---------|-------------|
| `/balance` | Check wallet balance |
| `/history` | Transaction history |
| `/send <to> <amount> <reason>` | Send LEZ tokens |
| `/approve <id>` | Approve pending spend |
| `/reject <id>` | Reject pending spend |
| `/upload <path> <label>` | Upload encrypted file |
| `/download <cid-or-label> <path>` | Download and decrypt file |
| `/files` | List stored files |
| `/skills` | List all 23 skills |
| `/status` | Agent status |
| `/discover` | Discover peer agents |
| `/help` | Show commands |
| `/quit` | Exit |

## Step 6: Run Tests

```bash
# Unit tests (208)
cd pilot-module && nix build .#unit-tests --extra-experimental-features 'nix-command flakes' -L

# Single-agent integration (28) — needs sequencer
cd .. && ./test-phases.sh

# Two-agent Docker (30 checks) — needs sequencer + Docker
pkill -9 -f logos_host_qt; pkill -9 -f logoscore
rm -f ~/.cache/storage/dht/providers/LOCK
./test-two-agents-docker.sh
```

Expected: "Results: 30 passed, 0 failed" (last full run 2026-08-28).

## Three agents on the public testnet, one per skill category

`agents/deploy-agent.sh` deploys one agent per default skill category against the public LEZ
testnet from a clean clone, runs that category's skills end to end, and asserts every step
(exit 1 the moment something does not do what it claims). It needs `nix`, `python3`, `curl`
and, for the two-agent roles, `docker` (one local nwaku relay). No keys, no wallet, no
sequencer of your own: each agent creates its identity and funds itself from the faucet.

```bash
# Blockchain agent: identity, faucet funding, a spend through the spending FSM, all read back
# from the chain (this is ./demo.sh). ~20-25 min, one faucet claim.
agents/deploy-agent.sh --role blockchain

# Storage agent: A uploads an encrypted file, lists it, shares the key with a second identity B
# over Logos Messaging; B receives the key, dials A's storage node, fetches the CID over the
# storage network and decrypts it byte-identical. Two daemons, two faucet claims.
agents/deploy-agent.sh --role storage

# Messaging agent: A -> B direct message; A creates a group whose sealed invite carries the
# group key, B joins; one AES-GCM group message each way — every message READ BACK on the
# receiving side (messagingInbox), not just sent. Two daemons, two faucet claims.
agents/deploy-agent.sh --role messaging
```

Each run leaves its evidence in `agents/out/<role>/`: the run log with `EVIDENCE` lines
(accounts, keys, CID, topics, message bodies), both daemons' logs, the storage nodes' own
logs, the inbox dumps, and for the storage role the original file next to B's fetched copy.
`.github/workflows/testnet-agents.yml` runs the three roles as three jobs (manual trigger and
weekly) and uploads that directory as the artifact `testnet-agent-<role>`; the identities from
those runs are the rows of `evidence/testnet-agents.tsv`, which `evidence/verify-testnet.sh`
re-checks against the chain.

Two agents on one host need distinct ports; the script sets them (`PILOT_TCP_PORT` for the
Waku node, `PILOT_STORAGE_API_PORT` / `PILOT_STORAGE_DISC_PORT` / `PILOT_STORAGE_LISTEN_PORT`
for the storage node) and dials the relay it starts. An agent receives messages and shared keys
only while it is open for hire (`agentOpenForHire`): closed means nobody can reach its inbox.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `PILOT_DATA_DIR` | `/tmp/pilot-data` | Agent data directory (SQLite, wallet, logs) |
| `PILOT_SEQUENCER_ADDR` | `http://127.0.0.1:3040` | LEZ sequencer endpoint; `pilot deploy --testnet` sets it to `https://testnet.lez.logos.co` (and `PILOT_CHAIN_WAIT_SECS=600`) unless already set |
| `PILOT_WAKU_ADDR` | `/ip4/127.0.0.1/tcp/30303` | Waku static peer |
| `PILOT_TCP_PORT` | `60000` | Waku relay TCP port |
| `PILOT_WAKU_MODE` | `Core` | `Core` (full relay) or `Edge` (lightweight) |
| `PILOT_NAT` | (auto) | NAT config: `extip:127.0.0.1` for WSL |
| `PILOT_WAKU_REST` | `http://127.0.0.1:8645` | The relay's REST API the pull path (`agentPoll`) reads |
| `PILOT_KEY_PASSPHRASE` | generated by `pilot deploy` | Passphrase the agent's private keys are encrypted with at rest (AES-256-GCM, PBKDF2). `deploy` generates one on first run and saves it at `$PILOT_DATA_DIR/.key-passphrase` (mode 0600) unless the variable is set; back that file up with `pilot.db`. Plaintext keys from older deploys are re-wrapped on the next start |
| `PILOT_STORAGE_API_PORT` | `5988` | Storage node REST port (loopback); distinct per agent on one host |
| `PILOT_STORAGE_DISC_PORT` | (libstorage default, 8090) | Storage node discovery UDP port; distinct per agent on one host |
| `PILOT_STORAGE_LISTEN_PORT` | (random) | Storage node libp2p listen port; fix it so a peer can dial `/ip4/<extip>/tcp/<port>` |
| `PILOT_STORAGE_NAT` | (auto) | Storage node NAT: `extip:<IP>` (the only other form libstorage accepts) |
| `PILOT_STORAGE_BOOTSTRAP` | — | Comma list of storage SPRs handed to the node as `bootstrap-node` |
| `ANTHROPIC_API_KEY` | — | Anthropic Claude API key |
| `PILOT_LLM_PROVIDER` | — | Headless `pilot deploy`: `anthropic`, `openai`, `deepseek`, `google`, `openrouter`, `groq` or `none` — skips the provider selector (which needs a terminal) |
| `PILOT_LLM_MODEL` | — | Headless `pilot deploy`: model id — skips the model selector |
| `OPENAI_API_KEY` | — | OpenAI API key |
| `DEEPSEEK_API_KEY` | — | DeepSeek API key |
| `GOOGLE_API_KEY` | — | Google Gemini API key (from aistudio.google.com) |
| `OPENROUTER_API_KEY` | — | OpenRouter API key |
| `GROQ_API_KEY` | — | Groq API key |

## Data Persistence

All agent data lives in `PILOT_DATA_DIR` (default `/tmp/pilot-data/`):

| File | Contains | Delete safely? |
|------|----------|---------------|
| `pilot.db` | Identity, encryption keys, files, config, LLM settings | NO — loses keys |
| `.logoscore/daemon/` | Daemon state (PID, tokens) | Yes — recreated on start |
| `daemon.log` | Module logs | Yes |
| `wallet_config.json` | Sequencer connection config | Yes — recreated |
| `wallet_storage/` | Wallet persistent storage | NO — loses wallet |

## Troubleshooting

**Module fails to load:** Run `./setup-modules.sh` to reinstall all modules.

**Daemon won't start:** Kill stale processes:
```bash
pkill -9 -f logos_host_qt; pkill -9 -f logoscore
rm -f ~/.cache/storage/dht/providers/LOCK
rm -rf /tmp/pilot-data/.logoscore/daemon
```

**LLM not responding:** Check API key is configured:
```
> /status
```
Look for `LLM` field. If "none", redeploy with `pilot deploy`.

**Upload works but download fails:** The first download initializes the storage module (2-3 seconds). If it times out, try again — subsequent downloads are instant.

**Agent freezes on first message:** The delivery module takes 30-60 seconds to connect to the Waku network on first use. Use slash commands (`/balance`, `/files`) while it warms up — these don't need delivery.

**"unknown CID" on download:** Make sure you're using the CID from `/files`, or download by label: `/download my-label /tmp/file.pdf`.

**"Identity generation failed" on deploy:** Check for stale Basecamp or logoscore processes. CLI and Basecamp cannot run at the same time — close one before using the other:
```bash
pkill -9 -f logos_host; pkill -9 -f logoscore
sleep 2
./pilot-cli/result/bin/pilot deploy
```

**Deploy fails repeatedly:** Modules crash on cold start (transient race). Reinstall and retry:
```bash
./setup-modules.sh
./pilot-cli/result/bin/pilot deploy
```

**CLI and Basecamp:** Both read/write the same `pilot.db`. Deploy once, use from either — but never run both at the same time.
