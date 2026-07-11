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
> send 50 LEZ to @bob for hosting
> discover other agents
> what can you do?
```

Note the `@bob` — payments in natural language must use a saved **contact**
(see [Contacts](#contacts)). Never paste raw wallet keys into chat: the agent
refuses to pass key material through the LLM, because retyped keys arrive
corrupted and the transfer fails.

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

## Contacts

A payment recipient is the payee's **full wallet keys JSON** (~200 characters):

```json
{"nullifier_public_key":"<64-hex>","viewing_public_key":"<66-hex>"}
```

A bare hex address is NOT a valid payee — it means "an account this wallet
owns" and the transfer fails for any peer. And pasting the long JSON into the
chat is fragile (the line editor can clip an early paste). So save it once as
a **contact**:

1. Get the payee's keys — on *their* agent it's one command:
   `logoscore call pilot getAgentNpk` (or ask their owner for the output).
2. Save the JSON as `~/.pilot/contacts/<name>.json`, e.g. `bob.json`.

From then on, both of these work:

```
> /send @bob 20 lunch
> send 20 LEZ to @bob for lunch        ← natural language, same result
```

The contact name is just the filename — the LLM only ever carries "@bob",
never the keys, so nothing can be corrupted. `@~/path/to/keys.json` also
works as a one-off file reference.

## Wallet

### Check Balance

```
> /balance
  pilot │   Agent Wallet
        │   Balance      100 LEZ
        │
        │   Fund this agent → 91996446eb22...
```

After a restart the first `/balance` can take a moment — the wallet replays
the chain before it answers (see `docs/troubleshooting.md`).

### Send Tokens

Below your spending threshold — executes autonomously:
```
> /send @bob 10 coffee
  pilot │   Transfer sent
        │   Amount  10 LEZ
```

Above your threshold — held, and you decide on the spot:
```
> /send @bob 100 large purchase

  Approval Required
  ❯ Approve — execute this transaction
    Reject — cancel and refund
    Skip — decide later (/pending)

  pilot │   Transaction Approved
        │   ID      a1b2c3d4
        │   Status  executed
```

If you pick Skip (or the hold came from the agent acting on its own), review
later with `/pending` and decide with `/approve <id>` or `/reject <id>`.
Held requests expire after 60 minutes — expired ones can no longer move funds.

`Approve Failed` means no tokens moved on *that* approval: the request was
unknown, already decided, expired, or its transfer failed — `/pending` and
`/balance` tell you which (see `docs/troubleshooting.md`).

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

Provider model lineups change over time — if chat replies fail with a model
error, check the provider's live model list and update with
`logoscore call pilot metaConfigure llm.model <id>` (no redeploy needed).
LLM errors never affect slash commands, which bypass the LLM entirely.

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
