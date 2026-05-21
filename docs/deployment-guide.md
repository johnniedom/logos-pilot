# Pilot Agent — Deployment Guide

Ref: [logoscore CLI](https://github.com/logos-co/logos-logoscore-cli) | [Basecamp](https://github.com/logos-co/logos-basecamp) | [Module Builder](https://github.com/logos-co/logos-module-builder)

## Prerequisites

- Linux (x86_64) or WSL2
- [Nix](https://nixos.org/download.html) with flakes enabled
- An LLM API key (optional — agent works without it in command-only mode)

## Quick Start (5 minutes)

```bash
# 1. Clone
git clone https://github.com/johnniedom/pilot.git
cd pilot

# 2. Build module
cd pilot-module && git init && git add -A
nix build --extra-experimental-features 'nix-command flakes'

# 3. Build logoscore CLI
nix build 'github:logos-co/logos-logoscore-cli' \
  --extra-experimental-features 'nix-command flakes' -o logoscore-cli

# 4. Deploy
export PATH="$PWD/logoscore-cli/bin:$PWD/../pilot-cli:$PATH"
pilot deploy --testnet
```

The wizard walks you through identity generation, LLM selection, owner binding, and funding.

## Step-by-Step

### 1. Build the Module

```bash
cd pilot-module
nix build --extra-experimental-features 'nix-command flakes'
```

Output: `result/lib/pilot_plugin.so` (37 methods, ~2.5 MB)

Verify with unit tests:
```bash
nix build .#unit-tests -L   # 44 tests
```

### 2. Get logoscore

```bash
nix build 'github:logos-co/logos-logoscore-cli' \
  --extra-experimental-features 'nix-command flakes' -o logoscore-cli
```

Output: `logoscore-cli/bin/logoscore`

### 3. Install Module as LGX Package

logoscore discovers modules via the LGX package manager — bare `.so` files won't be found.

```bash
# Build LGX package
nix build .#lgx --extra-experimental-features 'nix-command flakes' -o result-lgx

# Find lgpm
LGPM=$(find /nix/store -maxdepth 3 -name "lgpm" -path "*/bin/*" 2>/dev/null | head -1)

# Install into a modules directory
mkdir -p /tmp/pilot/modules
$LGPM install --file result-lgx/logos-pilot-module-lib.lgx \
  --modules-dir /tmp/pilot/modules --allow-unsigned
```

### 4. Set LOGOS_HOST_PATH

logoscore spawns each module in a subprocess via `logos_host`. You must tell it where to find the binary:

```bash
export LOGOS_HOST_PATH=$(find /nix/store -maxdepth 1 -name "*-logos-liblogos" -type d | head -1)/bin/logos_host
```

### 5. Test Headlessly (Inline Mode)

```bash
logoscore-cli/bin/logoscore \
  -m /tmp/pilot/modules -l pilot \
  -c "pilot.echo(hello)" --quit-on-finish
# Output: Method call successful. Result: echo: hello
```

### 6. Run as Daemon

```bash
# Start daemon
logoscore-cli/bin/logoscore -D -m /tmp/pilot/modules

# In another terminal
logoscore-cli/bin/logoscore load-module pilot
logoscore-cli/bin/logoscore call pilot echo "hello world"
logoscore-cli/bin/logoscore call pilot metaSkills
logoscore-cli/bin/logoscore call pilot metaStatus
```

### 5. Deploy with the Wizard

```bash
pilot deploy --testnet
```

Steps:
1. **Agent Identity** — generates keypair via `lez_wallet_module.createAccountPrivate()`
2. **LLM Provider** — arrow-key selector (Claude, OpenAI, Gemini, Local, OpenRouter)
3. **Owner Identity** — enter your Logos address from Basecamp > Settings
4. **Funding** — send LEZ to the displayed agent address
5. **Deploy** — publishes Agent Card, opens owner channel

### 6. Chat with Your Agent

```bash
pilot chat
```

### 7. Install in Basecamp (GUI)

For the full desktop experience with the 4-tab QML UI:

```bash
# Build portable package
nix build .#lgx-portable --extra-experimental-features 'nix-command flakes'

# Install via lgpm
lgpm install --file result/logos-pilot-module-lib.lgx \
  --modules-dir ~/.local/share/Logos/LogosBasecamp/modules \
  --allow-unsigned

# Launch Basecamp
LogosBasecamp
```

Or run Basecamp directly with the module:

```bash
nix build 'github:logos-co/logos-basecamp' \
  --extra-experimental-features 'nix-command flakes' -o basecamp

mkdir -p /tmp/pilot-data/modules
cp result/lib/pilot_plugin.so /tmp/pilot-data/modules/

./basecamp/bin/LogosBasecamp --user-dir /tmp/pilot-data
```

## Network Infrastructure

### LEZ Status (as of 2026-05-20)

The LEZ (L2) does **not** have public endpoints yet. It is deployed on internal staging only (v0.2.0-rc3).

The Logos L1 testnet is live but requires Discord credentials for faucet access:
- Dashboard: `https://testnet.blockchain.logos.co/web/`
- Faucet: `https://testnet.blockchain.logos.co/web/faucet/`
- Explorer: `https://testnet.blockchain.logos.co/web/explorer/`
- Credentials: request via [Discord](https://discord.com/channels/973324189794697286/1468535289604735038)

### Local Development (Required for Now)

Until LEZ has public endpoints, run a local sequencer using the pre-built Docker image:

```bash
# Pull the pre-built image (~1 GB download, ~2 minutes)
docker pull ghcr.io/logos-blockchain/logos-blockchain:devnet

# Run the sequencer
docker run --rm -p 8080:8080 \
  -e SEQUENCER_LISTEN_ADDR=0.0.0.0:8080 \
  -e SEQUENCER_DB_PATH=/data/sequencer.db \
  -e SEQUENCER_SIGNING_KEY_PATH=/data/sequencer.key \
  -e SEQUENCER_INITIAL_BALANCE=1000 \
  -v sequencer-data:/data \
  --entrypoint /usr/bin/logos-blockchain-demo-sequencer \
  ghcr.io/logos-blockchain/logos-blockchain:devnet

# Verify it's running (in another terminal)
curl http://localhost:8080
```

> **Note:** Building from source (`docker compose up` in the `logos-execution-zone` repo) is NOT recommended — the full LEZ sequencer images are pushed to a private registry and the source build requires significant resources. The demo sequencer in the devnet image provides the same functionality needed for Pilot.

Once running, `pilot deploy --testnet` connects to `localhost:8080`. When LEZ launches publicly, this becomes a config change (URL swap only).

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `PILOT_LLM_PROVIDER` | LLM provider (`anthropic`, `openai`) | auto-detect |
| `PILOT_LLM_MODEL` | Model identifier | `claude-sonnet-4-6` / `gpt-4o` |
| `ANTHROPIC_API_KEY` | Claude API key | — |
| `OPENAI_API_KEY` | OpenAI-compatible API key | — |
| `OPENAI_BASE_URL` | Custom endpoint (Ollama, LM Studio) | `https://api.openai.com/v1` |
| `PILOT_DATA_DIR` | Persistent data directory | `/tmp/pilot-data` |

## Verification

```bash
pilot verify
```

Generates a human-readable + JSON evidence report: identity, balance, all 21 skills status, peer agents, transaction history.

## Troubleshooting

**Module fails to load:** Check dependencies in `metadata.json`. The module requires `lez_wallet_module`, `delivery_module`, `storage_module`, `chat_module` to be available.

**LLM not responding:** Verify API key is set (`echo $ANTHROPIC_API_KEY`). Check `pilot status` for LLM configuration.

**Spending approval not received:** Ensure owner channel is established (`/status` shows `owner_channel` is non-empty). Check chat_module is running.
