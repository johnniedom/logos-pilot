# Pilot Agent — Security Model

## Principle: Ambiguity Defaults to Inaction

The agent NEVER executes a financial transaction when in doubt. If the owner's intent is unclear, the agent holds the request and asks for explicit confirmation.

## What the Agent Can Do Without Owner Approval

- Read its own wallet balance
- List stored files
- Query on-chain program state (read-only)
- Discover peer agents on the network
- Respond to A2A task requests (if the skill is free)
- Send messages to known contacts
- Report its own status

## What Requires Owner Approval

- **Token transfers above the per-transaction threshold** — held in PENDING state, owner notified via encrypted chat
- **Token transfers above the per-period cumulative limit** — even if individual amounts are below per-tx threshold
- **Program deployments** — always held regardless of amount (high cost, irreversible)
- **Program calls with non-zero cost** — subject to spending threshold

## Spending Threshold Mechanism

Two configurable limits:
- `spendLimitPerTx` — maximum LEZ per single transaction (default: 100)
- `spendLimitPerPeriod` — maximum LEZ over `spendPeriodSeconds` (default: 500/day)

### 9-State FSM

```
CREATED → HELD → NOTIFIED → [APPROVED → EXECUTING → COMPLETED]
                           → [REJECTED]
                           → [EXPIRED]
                           → [FAILED]
```

- **CREATED** — request generated, limits checked
- **HELD** — exceeds threshold, waiting for notification delivery
- **NOTIFIED** — owner received the approval card via encrypted chat
- **APPROVED** — owner sent `/approve <id>`
- **REJECTED** — owner sent `/reject <id>`
- **EXPIRED** — owner didn't respond within the timeout
- **EXECUTING** — on-chain transaction submitted
- **COMPLETED** — transaction confirmed
- **FAILED** — transaction failed (funds not deducted)

All state transitions are persisted to SQLite WAL before execution. On crash recovery, the agent re-sends notifications for any requests stuck in HELD or NOTIFIED state.

## Key Management

- Agent identity: shielded LEZ account via `lez_wallet_module.createAccountPrivate()`
- NPK (Nullifier Public Key) used as the agent's public identity
- ISK (Identity Secret Key) never leaves the wallet module
- No custom key generation — uses the same `key_protocol::KeyChain` as all Logos identities

## Encryption

### File Encryption (AES-256-GCM)
- Random 256-bit key + 96-bit IV per file
- Encrypted before upload, decrypted after download
- Key stored in SQLite `stored_files.file_key_encrypted` as hex (`key:iv:tag`)
- File sharing: key is ECIES-encrypted to recipient's NPK before delivery

### Message Encryption (ECIES)
- Ephemeral EC keypair (secp256k1) per message
- ECDH with recipient's NPK → SHA256 KDF → AES-256-GCM
- Ciphertext envelope: `{ephemeral_pubkey, ciphertext, iv, tag}`
- Applied to: A2A inbox messages, A2A reply messages, direct messaging
- NOT applied to: discovery topic (Agent Cards are intentionally public)

### Owner Channel
- End-to-end encrypted by `chat_module` (not Pilot's responsibility)
- Pilot calls `chat_module.sendMessage()` — encryption is handled by the transport

## LLM Security

- API keys stored as environment variables, never in SQLite or on-chain
- LLM system prompt contains only: available commands, current agent state
- LLM output is parsed as structured JSON — the agent validates the action before executing
- If LLM returns garbage, the agent responds with "I couldn't process that"
- Agent works without LLM (command-only mode) — LLM is not a security dependency

## Network Attack Surface

| Vector | Mitigation |
|--------|-----------|
| Eavesdropping on A2A | ECIES encryption on all inbox/reply topics |
| Spoofed Agent Cards | Cards will be ISK-signed (v2) |
| Replay attacks on tasks | Unique task IDs + timestamps in `_logos` envelope |
| DoS via task spam | Rate limiting at Waku relay layer |
| Malicious skill execution | Skills run in-process but are isolated by return-value interface |

## What the Agent Cannot Do

- Access the owner's wallet or keys
- Bypass the spending threshold
- Send unencrypted messages to agent inboxes
- Execute transactions while the owner channel is down (for above-threshold amounts)
- Modify other modules' state or configuration
