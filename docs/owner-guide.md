# Pilot Agent — Owner Guide

## Interacting With Your Agent

After deployment, you communicate with your agent through two channels:

### 1. Terminal Chat (`pilot chat`)

```bash
pilot chat
```

A REPL where you type natural language or slash commands. The LLM interprets natural language and dispatches the right skill.

### 2. Basecamp Chat (Owner Channel)

Open Basecamp → Chat tab → your agent's conversation. Same commands work here — messages go through end-to-end encrypted chat_module.

## Commands

### Wallet
| Command | Description |
|---------|-------------|
| `/balance` | Show current LEZ balance |
| `/history` | Recent transaction history |
| `/send <address> <amount> <reason>` | Send LEZ tokens |

### Storage
| Command | Description |
|---------|-------------|
| `/upload <path> <label>` | Encrypt and upload a file |
| `/download <cid> <path>` | Download and decrypt a file |
| `/files` | List all stored files |

### Spending Approval
| Command | Description |
|---------|-------------|
| `/approve <id>` | Approve a pending spend request |
| `/reject <id>` | Reject a pending spend request |

### Agent Management
| Command | Description |
|---------|-------------|
| `/status` | Agent status (balance, LLM, channels) |
| `/skills` | List all available skills |
| `/discover` | Find peer agents on the network |

### Natural Language (requires LLM)

If an LLM provider is configured, you can speak naturally:

```
you › what's my balance?
pilot › Your balance is 150 LEZ

you › upload my-report.pdf as quarterly report
pilot › Encrypting and uploading... ✓ Stored as CID: bafy2bza...

you › yeah go ahead
pilot › Approved. Executing transfer of 10 LEZ to abc123...
```

The agent parses your intent and maps it to the appropriate command.

## Spending Approval Flow

When the agent needs to spend more than the per-transaction limit:

```
pilot › Spend request created:
        ID: a3f7b2c1
        Recipient: abc123...
        Amount: 250 LEZ
        Reason: Program deployment
        
        /approve a3f7b2c1
        /reject a3f7b2c1
```

- Below threshold → executes automatically, you're notified after
- Above threshold → held, you must approve or reject
- No response → expires after timeout (funds stay safe)

### Configuring Limits

```bash
pilot configure spending.per_transaction_limit 200
pilot configure spending.per_period_limit 1000
pilot configure spending.period_seconds 86400    # 24 hours
```

## LLM Configuration

### During Deployment

The `pilot deploy` wizard lets you choose with arrow keys:
- Claude (Anthropic)
- OpenAI / GPT
- Gemini (Google)
- Local (Ollama / LM Studio)
- OpenRouter

### After Deployment

```bash
pilot configure llm.provider anthropic
pilot configure llm.model claude-sonnet-4-6

# Or via environment variables
export ANTHROPIC_API_KEY=sk-...
export PILOT_LLM_PROVIDER=anthropic
export PILOT_LLM_MODEL=claude-sonnet-4-6
```

### Without LLM

The agent works in command-only mode if no LLM is configured. All slash commands work — you just can't use natural language.

## Verification

Generate an evidence report for evaluators or your own records:

```bash
pilot verify
```

Outputs: agent address, balance, skill status (21/21), peer count, transaction history, and machine-parseable JSON.
