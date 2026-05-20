# Pilot CLI

Deployment and management CLI for the Pilot sovereign agent on LEZ.

## Quick Start

```bash
# Make executable (first time only)
chmod +x pilot-cli/pilot pilot-cli/lib/*.sh

# Deploy to testnet
./pilot-cli/pilot deploy --testnet

# Check agent status
./pilot-cli/pilot status

# Collect verification evidence
./pilot-cli/pilot verify

# Discover peer agents
./pilot-cli/pilot discover
```

## Commands

### `pilot deploy [--testnet|--mainnet]`

Interactive 5-step deployment wizard:

1. **Agent Identity** — generates or loads keypair via `pilot.initialize()`
2. **LLM Provider** — arrow-key selector for Claude/OpenAI/Gemini/Local/OpenRouter + API key input
3. **Owner Identity** — bind your Logos address from Basecamp
4. **Funding** — display agent address, poll for incoming LEZ tokens
5. **Deploy** — publish Agent Card, establish owner channel

### `pilot verify [--json-only]`

Collect evidence for evaluators. Shows:
- Agent identity (NPK, account ID)
- Wallet balance and transaction history
- All 21 skill registration status
- Agent Card publication status
- Owner channel connection
- Discovered peers
- Uptime

Outputs both a human-readable report and machine-parseable JSON.

### `pilot discover [topic] [--timeout SECS] [--json]`

Query the discovery topic and display found agents in a table.

### `pilot configure <key> <value>`

Update agent configuration. Keys:
- `llm.provider` — anthropic, openai, google, local, openrouter
- `llm.model` — model identifier
- `owner.address` — owner's Logos address
- `spend.per_tx` — per-transaction spending limit
- `spend.per_period` — per-period spending limit
- `spend.period` — spending period in seconds

### `pilot status [--json]`

Show current agent status, identity, balance, and uptime.

## Environment Variables

| Variable | Description | Default |
|---|---|---|
| `PILOT_DATA_DIR` | Agent data directory | `/tmp/pilot-data` |
| `PILOT_MODULE_PATH` | Module library path | `./result/lib` |
| `PILOT_LLM_PROVIDER` | LLM provider | (set during deploy) |
| `PILOT_LLM_MODEL` | LLM model | (set during deploy) |
| `ANTHROPIC_API_KEY` | Anthropic API key | — |
| `OPENAI_API_KEY` | OpenAI API key | — |
| `GOOGLE_API_KEY` | Google API key | — |
| `OPENROUTER_API_KEY` | OpenRouter API key | — |

## Requirements

- Bash 5+
- `logoscore` from the Logos SDK
- `jq` (optional, improves discovery output formatting)

## Architecture

```
pilot-cli/
├── pilot              # Main entry point — subcommand dispatch
├── lib/
│   ├── common.sh      # Colors, logging, pilot_call() wrapper
│   ├── arrow-select.sh # Arrow-key selector widget (inquirer-style)
│   ├── deploy.sh      # 5-step deployment wizard
│   ├── verify.sh      # Evidence collection for evaluators
│   └── discover.sh    # Agent discovery on LEZ network
└── README.md
```

The CLI communicates with the Pilot C++ module exclusively through `logoscore`:
```bash
logoscore -m <module-path> -l pilot -c "pilot.methodName(args)"
```

This is a management tool, separate from the module itself.
