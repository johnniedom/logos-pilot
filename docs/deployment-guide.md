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
curl -s http://localhost:8080/        # sequencer — should return 404 (alive)
docker ps                              # should show pilot-nwaku + sequencer
```

## Step 2: Build

```bash
# Build the C++ module
cd pilot-module
git init && git add -A
nix build --extra-experimental-features 'nix-command flakes'

# Run unit tests (44/44)
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

```bash
./setup-modules.sh
```

This installs pilot + all dependency modules (wallet, delivery, storage, chat, capability) from the nix cache into `/tmp/pilot-logoscore/modules/`.

## Step 4: Deploy

```bash
./pilot-cli/result/bin/pilot deploy
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
        │ discover other agents, and execute on-chain transactions. 21 skills total.
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
| `/skills` | List all 21 skills |
| `/status` | Agent status |
| `/discover` | Discover peer agents |
| `/help` | Show commands |
| `/quit` | Exit |

## Step 6: Run Tests

```bash
# Unit tests (44)
cd pilot-module && nix build .#unit-tests --extra-experimental-features 'nix-command flakes' -L

# Single-agent integration (28) — needs sequencer
cd .. && ./test-phases.sh

# Two-agent Docker (14) — needs sequencer + Docker
pkill -9 -f logos_host_qt; pkill -9 -f logoscore
rm -f ~/.cache/storage/dht/providers/LOCK
./test-two-agents-docker.sh
```

Expected: 86/86 pass.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `PILOT_DATA_DIR` | `/tmp/pilot-data` | Agent data directory (SQLite, wallet, logs) |
| `PILOT_SEQUENCER_ADDR` | `http://127.0.0.1:8080` | LEZ sequencer endpoint |
| `PILOT_WAKU_ADDR` | `/ip4/127.0.0.1/tcp/30303` | Waku static peer |
| `PILOT_TCP_PORT` | `60000` | Waku relay TCP port |
| `PILOT_WAKU_MODE` | `Core` | `Core` (full relay) or `Edge` (lightweight) |
| `PILOT_NAT` | (auto) | NAT config: `extip:127.0.0.1` for WSL |
| `ANTHROPIC_API_KEY` | — | Anthropic Claude API key |
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
