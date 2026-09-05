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

## From a Separate Machine: `pilot-owner`

`pilot chat` and the Basecamp plugin talk to the agent through its local daemon. `pilot-owner`
(in `pilot-owner/`) talks to it the way the prize asks for: from a separate program, over Logos
Messaging, with nothing between the two but a Waku relay. It has the agent's own encryption and
signing code compiled in, so what it sends is exactly what the agent verifies: every message is
sealed to the agent's key and signed with yours, with a strictly increasing nonce; the agent
answers on the same topic, sealed to your key.

```bash
nix build ./pilot-owner -o result-owner            # builds and runs its self-test
OWNER=./result-owner/bin/pilot-owner

$OWNER init                                        # makes your keypair, prints your public key
logoscore call pilot metaConfigure owner.npk <your public key>   # bind the agent to you (or PILOT_OWNER_NPK= at deploy)
logoscore call pilot establishOwnerChannel
logoscore call pilot agentCard > card.json         # the agent's key lives in the card
$OWNER pair card.json <agent account id> --relay http://127.0.0.1:8645

$OWNER send "/balance"                             # sign, seal, publish
$OWNER listen                                      # the agent's replies from the last 15 minutes
$OWNER listen --follow                             # keep reading
```

The agent reads its owner topic from the relay when it polls (`logoscore call pilot agentPoll`,
or `pilot poll`), so a reply arrives on the next poll, not instantly. Commands are the slash
commands above; `/help` lists the ones the agent executes over the channel. A spend above your
limit is held and the agent tells you: `... Approval required. /approve <id>`; send
`/approve <id>` and it executes, or `/reject <id>`.

Where the two sides run does not matter as long as both reach the same relay: the relay's REST
API is the client's only network dependency. Your private key is in `~/.pilot-owner/state.json`
(mode 0600; `PILOT_OWNER_HOME` moves it). The agent pins your signing key on first contact and
drops anything signed by another key, and drops replays (a nonce not above the last one).

`agents/owner-channel.sh` runs the whole thing against the public testnet from a clean clone:
agent and client on one host sharing only the relay, `/balance` answered, a 101-LEZ spend held
and announced, approved from the client, the transaction read back from the chain
(`.github/workflows/owner-channel.yml`).

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
