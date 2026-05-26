# Pilot Agent — Owner Guide

## Getting Started

After deploying your agent (`pilot deploy`), start chatting:

```bash
./pilot-cli/result/bin/pilot chat
```

The agent asks your name on first use and remembers it. Type naturally or use slash commands.

## Talking to Your Agent

The agent understands natural language. Ask it things like:

```
> what's my balance?
> upload this file to storage
> what files do I have?
> send 50 LEZ to <address> for hosting
> discover other agents
> what can you do?
```

The agent dispatches the right action and summarizes the result. If something fails, it tells you what went wrong and suggests a fix.

## Slash Commands

For direct control, use slash commands — they bypass the LLM and go straight to the module:

| Command | What it does |
|---------|-------------|
| `/balance` | Shows wallet balance |
| `/history` | Shows transaction history |
| `/send <to> <amount> <reason>` | Sends LEZ tokens |
| `/approve <id>` | Approves a pending spend request |
| `/reject <id>` | Rejects a pending spend request |
| `/upload <path> <label>` | Encrypts and uploads a file |
| `/download <label-or-cid> <path>` | Downloads and decrypts a file |
| `/files` | Lists all stored files with CIDs |
| `/skills` | Lists all 21 agent skills |
| `/status` | Shows agent status, LLM config, account |
| `/discover` | Discovers other agents on the network |
| `/help` | Shows all commands |
| `/quit` | Exits the chat |

## File Management

### Upload

```
> /upload /path/to/document.pdf my-document
  pilot │   Uploaded  my-document
        │   CID  zDvZRwzm3EEJ...
        │   Encrypted  yes
```

Files are encrypted with AES-256-GCM before upload. The encryption key is stored locally in your agent's database.

### List Files

```
> /files
  pilot │   Stored Files
        │   my-document
        │     CID  zDvZRwzm3EEJDkF2Em...
        │   contract-draft
        │     CID  zDvZRwzkz4tYVp8VBp...
```

### Download

By label (recommended):
```
> /download my-document /tmp/my-document.pdf
  pilot │   Downloaded  /tmp/my-document.pdf
        │   Decrypted  yes
```

By CID:
```
> /download zDvZRwzm3EEJDkF2Em... /tmp/file.pdf
```

WSL users: files on the Windows desktop are at `/mnt/c/Users/<name>/Desktop/`.

## Wallet

### Check Balance

```
> /balance
  pilot │   Account  91996446eb22...
        │   Balance  1000 LEZ
```

### Send Tokens

Below your spending threshold — executes immediately:
```
> /send <recipient> 50 payment for services
  pilot │   Transfer sent
        │   Amount  50 LEZ
```

Above your threshold — held for your approval:
```
> /send <recipient> 500 large purchase
  pilot │   Spend request created
        │   ID  a1b2c3d4
        │   State  HELD

> /approve a1b2c3d4
```

### Spending Limits

The agent enforces per-transaction and per-period limits. Configure via:
```
> /status    # see current limits
```

Default: 100 LEZ per transaction, 500 LEZ per period (24 hours).

## Agent Discovery

Find other agents on the network:
```
> /discover
  pilot │   Agents (0)
        │   No agents found — subscribed for live cards
```

Your agent publishes its own Agent Card during deploy. Other agents can discover you the same way.

## LLM Configuration

The LLM provider is selected during `pilot deploy`. To change it, redeploy:
```bash
./pilot-cli/result/bin/pilot deploy
```

Supported providers: Anthropic (Claude), OpenAI (GPT), DeepSeek, Google (Gemini), OpenRouter, Groq.

The API key and provider are stored in your agent's database and restored automatically on every chat session.

## Important: Protect Your Data

Your agent's database (`pilot.db`) contains:

- Your identity (NPK keypair)
- Wallet connection
- File encryption keys
- LLM API key
- Owner configuration

Never delete `pilot.db`. If lost, uploaded files cannot be decrypted and your identity is gone.

Safe to delete:
- `.logoscore/daemon/` — daemon state, recreated on start
- `daemon.log` — logs

## Tips

- Slash commands are faster than natural language for actions — no LLM round-trip
- The first storage or messaging command takes 2-3 seconds (module init)
- The agent remembers your conversation (20 turns) — refer to previous messages naturally
- If the agent seems stuck, Ctrl+C and restart `pilot chat`
- Check `/status` to verify LLM, account, and initialization state
