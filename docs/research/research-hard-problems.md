# LP-0008 — Four Hard Problems: Research Findings

> Research conducted 2026-05-14. All four problems investigated in parallel.
> These findings inform engineering decisions before writing code.

---

## 1. A2A Transport Binding over Waku

### The problem
A2A assumes HTTP (send request, get response on same connection). Waku is fire-and-forget pub/sub. We need to bridge request/response semantics over one-way messaging. Nobody has done this in the Logos ecosystem.

### The answer: NATS-style ephemeral reply topics
Stolen from NATS request-reply pattern (battle-tested in production at scale):

1. Client generates a unique reply topic and subscribes to it
2. Client publishes the request to the agent's inbox topic, including the reply topic in the message envelope
3. Agent processes the request, publishes response to the reply topic
4. Client receives response, unsubscribes from reply topic

This is the "Return Address" pattern from Enterprise Integration Patterns. MQTT 5.0 does the same thing with response topic headers.

### Topic structure

```
Discovery (public, broadcast):
  /pilot/1/discovery/proto              — Agent Cards published here

Agent inbox (per-agent, incoming requests):
  /pilot/1/inbox-<4-byte-npk-hash>/proto — bucketed by hash of agent's NPK for k-anonymity

Reply channel (per-request, ephemeral):
  /pilot/1/reply-<request-uuid>/proto   — caller subscribes before sending request

Task stream (per-task, long-lived):
  /pilot/1/task-<task-id>/proto         — streaming updates for SubscribeToTask
```

### Message envelope
Standard JSON-RPC 2.0 body (byte-for-byte A2A compatible) plus a `_logos` extension:

```json
{
  "jsonrpc": "2.0",
  "id": "req-uuid",
  "method": "message/send",
  "params": { "...standard A2A..." },
  "_logos": {
    "sender_npk": "compressed-pubkey-hex",
    "reply_topic": "/pilot/1/reply-<uuid>/proto",
    "signature": "ISK-signature",
    "timestamp": 1715700000000
  }
}
```

A gateway can strip `_logos` and forward to any HTTP A2A client. Full interop preserved.

### Encryption per topic type
- **Discovery**: plaintext (cards are public, signed with ISK for authenticity)
- **Agent inbox**: ECIES to agent's NPK (only target agent can read)
- **Reply topic**: ECIES to sender's NPK (only requester can read response)
- **Task stream**: symmetric key (exchanged during task handshake, allows multiple subscribers)

### Handling the race window
Subscribe-before-send has a small race: if the agent responds before the Filter subscription is fully active, the response is lost. Mitigation: use Waku Store to replay missed messages on the reply topic after a 2s grace period. Primary path is low-latency Filter; Store is the safety net.

### Streaming (A2A tasks with multiple status updates)
Reply topic doubles as the stream channel. Agent publishes multiple `StreamResponse` messages. Client sorts by `TaskStatus.timestamp` (Waku doesn't guarantee ordering across service nodes). Task reaches terminal state → both sides unsubscribe.

### Key risk
NPK/ISK key derivation for Waku encryption is not yet pinned in the Logos identity spec. Confirm exact KDF with Logos team before hardcoding.

---

## 2. Agent Wallet/Identity Model

### The problem
Does the agent need its own full LEZ account, or can we use delegated access like banking (authorized signatory with spending limits)?

### The answer: own account (the spec is right)

The banking analogy breaks down because LEZ has no infrastructure-level delegation primitive.

**In banking:** The bank's system enforces limits. An authorized signatory can only spend within bounds because the bank rejects over-limit transactions at the infrastructure level.

**In LEZ:** Shielded accounts use ZK proofs. To sign a private transaction, you need the SecretSpendingKey (SSK). There is no way to give "limited signing authority" — you either hand over the SSK (full custody transfer) or you don't. "Delegated access" = "shared custody of the owner's key" = worst possible configuration.

**Three approaches compared:**

- **Own account (spec requirement)**: Agent generates its own KeyChain. Clean identity, simple signing, application-layer spending limits. Risk: agent SSK is a catastrophic secret, but blast radius is limited to agent's funded balance.

- **Delegated access (banking model)**: Agent holds owner's SSK. Fails the spec ("own shielded LEZ account" is a success criterion). Blast radius of compromise = owner's entire wallet. Strictly worse.

- **Hybrid (own account + on-chain SPEL enforcement)**: Agent has own account, but a custom SPEL program enforces spending limits on-chain. Even a compromised agent can't exceed limits because the sequencer rejects the transaction. Owner can freeze the program remotely. Best engineering but adds complexity.

### Decision
**Own account with application-layer spending threshold for v1.** Document the path to on-chain enforcement (SPEL program) in `docs/security-model.md`. If LP-0002 (multisig) ships before our submission, integrate it for above-threshold co-signing.

### Why the spec chose this
1. Privacy: agent is indistinguishable from any other account holder on-chain
2. Independence: can receive payments from other agents (paid-skill marketplace) without hitting owner's account
3. Identity: can join token-gated groups as itself, not impersonating the owner
4. An on-chain delegation registry would leak the owner-agent relationship, contradicting LEZ's privacy model

---

## 3. Getting 5 Third-Party Deployments

### The problem
5 real people outside our team must deploy their own agent on LEZ testnet, each demonstrating at least one skill autonomously.

### Why the spec requires this
Three things in order of importance:
1. **Proves the software works outside the author's machine** — proxy for "your demo script actually works from clean environment"
2. **Proves the deployment UX is viable** — Logos is funding infrastructure, not art projects
3. **Shows minimum ecosystem traction** — Logos Foundation needs to show stakeholders that prize-funded software gets used

It's NOT a growth metric. Five is deliberately low. It's a "prove this isn't vaporware" check.

### The answer: 5-minute quickstart + 15-20 targeted DMs

**The formula:**
1. Make deployment take 5 minutes (Docker Compose + setup script)
2. Ask 15-20 specific people (mostly Logos Discord)
3. Hand-hold the first 2 (they're QA, not just numbers)
4. Make evidence collection automatic (`pilot verify` + GitHub Issue template)
5. Budget $100 for bounties as insurance

**Minimum viable onboarding flow:**

```bash
git clone https://github.com/johnniedom/pilot-quickstart
cd pilot-quickstart
./setup.sh                              # Docker Compose: Waku + Codex + LEZ
pilot deploy --testnet --config examples/file-vault.toml
pilot verify                            # outputs agent address, balance, skill status
```

**Who to ask (in order of conversion likelihood):**
- **Logos Discord builders** (warmest): DM 15-20 in #builder-hub. Expect 3-5 conversions with pair-programming offer.
- **Friends/classmates**: 2-3. They count as "outside submitting team" as long as they're not listed as co-authors.
- **Bounty deployers**: $20 USDC each if Tiers 1-2 fall short. Activate only if needed at T-7 days.

**Evidence package for evaluators:**
- On-chain records: table of 5 agent addresses + transaction hashes
- Agent Card discovery: evaluator runs `pilot discover` and sees 5+ agents
- GitHub Issues: each deployer opens an issue from their own account with `pilot verify` output
- (Optional) signed attestation with agent's ISK

**Critical timeline integration:**
- Day 7-9: write quickstart guide + Docker Compose + setup.sh (~12 hours)
- Day 13-15: first 2 friendly deployers (QA round — fix what they break)
- Day 16-18: post in Logos Discord, DM 15-20 builders
- Day 19-21: deploy-3, deploy-4, deploy-5 land. Bounty fallback if needed.
- Day 22-23: collect evidence, build submission table

Start at Day 7-9, NOT after code-complete. Writing the guide forces good deployment UX while the code is still malleable.

**Budget:** $0-100 worst case. Docker Hub free tier, GitHub free, testnet tokens from faucet.

---

## 4. Spending Approval over Encrypted Messaging

### The problem
When the agent wants to spend above the owner's limit, it messages the owner and waits for yes/no over encrypted Waku messaging. Must handle crashes, timeouts, network drops, multiple pending transactions, and garbled replies.

### Patterns stolen from existing systems

- **Banking authorization holds**: reserve funds immediately, release on timeout. Prevents over-commitment.
- **Gnosis Safe tx queue**: off-chain approval collection, on-chain execution. Separate the two concerns.
- **TimelockController**: explicit state enum with well-defined transitions.
- **GitHub Actions protection rules**: timeout-based expiry, designated reviewer approval.
- **Tiered autonomy**: below threshold = autonomous, above = approval required, some actions = forbidden regardless of amount.

### State machine (9 states)

```
CREATED → HELD → NOTIFIED → APPROVED → EXECUTING → COMPLETED
                    │           │            │
                    │           ▼            ▼
                    │       REJECTED      TX_FAILED
                    ▼
              NOTIFY_FAILED → EXPIRED
```

| State | Meaning |
|-------|---------|
| CREATED | Threshold check not yet performed. In-memory only. |
| HELD | Exceeds threshold. Persisted to SQLite. Available balance reduced. |
| NOTIFIED | Approval request sent to owner via Waku. Retry timer active. |
| APPROVED | Owner approved. Ready for on-chain execution. |
| REJECTED | Owner rejected. Terminal. Hold released. |
| EXECUTING | On-chain tx submitted. Waiting for confirmation. |
| COMPLETED | On-chain tx confirmed. Terminal. Hold cleared. |
| TX_FAILED | On-chain tx reverted. Terminal. Hold released. Owner notified. |
| NOTIFY_FAILED | All retries exhausted. Owner unreachable. |
| EXPIRED | TTL elapsed without response. Terminal. Tx NOT executed. |

**Critical rule: ambiguity defaults to inaction, never execution.**

### Available balance calculation

```
available = on_chain_balance - SUM(amount WHERE state IN (HELD, NOTIFIED, APPROVED, EXECUTING))
```

Prevents over-commitment when multiple transactions are pending simultaneously.

### Period limit tracking

```sql
SELECT SUM(amount) FROM completed_transactions
WHERE completed_at > datetime('now', '-' || period_seconds || ' seconds')
```

Plus in-flight holds. If cumulative exceeds period limit, approval required even for below-per-tx-threshold amounts.

### Storage: SQLite WAL mode

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;    -- belt and suspenders for financial data
```

- Crash recovery is automatic (SQLite replays WAL on open)
- Concurrent reads while writing (agent handles multiple skills)
- Single file at `{data_dir}/pilot/approvals.db`
- Use `rusqlite` crate directly (no Qt SQL layer needed from Rust)

### Owner channel UX

**Approval request format (phone-readable):**

```
APPROVAL NEEDED [#a3f7]

wallet.send wants to transfer 5,000 LEZ to lpub_8a3f...c412

Amount: 5,000 LEZ
Recipient: lpub_8a3f7b2c...c412
Reason: Payment for translation task

Your limits: 1,000 LEZ per tx / 10,000 LEZ per day
Already spent today: 3,200 LEZ

Expires in 58 minutes.

Reply:
  /approve a3f7
  /reject a3f7
```

**Command parsing:**
- `/approve <id>` (aliases: yes, ok, y)
- `/reject <id>` (aliases: no, deny, n)
- `/pending` — list all pending
- `/limits` — show current thresholds
- Garbled input → friendly nudge, NOT rejection
- Optional: LLM fallback for natural language ("yeah go ahead")

### Crash recovery sequence

On startup:
1. Open SQLite (WAL auto-recovery)
2. Query all non-terminal rows
3. HELD → check expiry, re-send notification
4. NOTIFIED → check expiry, re-send notification
5. APPROVED → re-submit on-chain tx
6. EXECUTING → check on-chain status (confirmed/reverted/unknown)
7. Catch up on Waku Store for any owner replies received during downtime
8. Notify owner of recovery summary

### Idempotency
- `/approve a3f7` when already APPROVED/COMPLETED → no-op with confirmation
- `/reject a3f7` when already EXECUTING/COMPLETED → rejected with explanation
- Duplicate notification on restart → owner sees it twice, but approving twice is harmless

---

## Cross-cutting: Implementation Order

These four problems map to the D10 order of operations from `decisions.md`:

1. **Module skeleton + identity + owner channel** — touches problems 2 (wallet identity) and 4 (owner channel foundation)
2. **Storage skill** — reuse LP-0017, no hard-problem dependency
3. **Messaging + meta skills** — standard plumbing
4. **Wallet + spending threshold** — problem 4 (full state machine + SQLite + UX)
5. **A2A binding + agent.* skills** — problem 1 (Waku transport binding)
6. **Basecamp UI** — owner chat interface for problem 4
7. **Quickstart + distribution** — problem 3 (starts at step 4-5, not after step 7)

The quickstart work runs in parallel with coding from step 4 onward.
