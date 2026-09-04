# Pilot Agent — Security & Trust Model

This document describes what the Pilot agent will and will not do on its own, who
it trusts, and how that trust is enforced in code. It is written to be honest: where
a guarantee is partial or a feature is owner-gated rather than autonomous, that is
stated plainly. A judge or operator can read the source in `pilot-module/src/` and
hold this document to it.

## Core Principle: Ambiguity Defaults to Inaction

The agent NEVER executes a financial transaction when in doubt. If intent is unclear,
or an above-threshold spend cannot be confirmed by the owner, the agent holds the
request and does nothing rather than guessing. Inaction is always the safe default;
autonomous execution is the narrow, explicitly-gated exception.

---

## 1. Identity

The agent holds **two** distinct cryptographic identities, by design:

1. **A shielded LEZ account (payment identity).** Created via the wallet module
   (`lez_core`) `create_account_private` / `get_private_account_keys`.
   This yields the agent's `npk` (nullifier public key) and viewing key. The `npk`
   is the agent's on-chain payment identity — it is what an A2A peer pays, and what
   appears as `_logos.payout` and `_logos.npk` on the agent's Agent Card. The
   spending secret key never leaves the wallet module. No custom key generation is
   used — identity comes from the same wallet primitives as every other LEZ account.

2. **A separate ECIES messaging keypair (`agentEciesPub_` / `agentEciesPriv_`).**
   A secp256k1 keypair the agent generates and holds itself
   (`generateECIESKeypair()` in `pilot_crypto.cpp`). This key is the agent's
   **messaging and signing identity**: it is the address its A2A inbox is keyed on,
   it is published as `_logos.signing_key` on the Agent Card, and it is the key that
   signs cards and replies. Encryption to/from the agent is ECIES to this public key.

Keeping payment identity (the shielded `npk`) separate from messaging/signing
identity (the ECIES key) means the on-chain spending key is never used as a transport
or signing key, and a compromise of the messaging layer does not expose spend
authority.

---

## 2. The Owner Channel

The owner channel is the trusted, private link between the agent and its human owner.
It carries spend-approval prompts, status, and owner commands (`/approve`, `/reject`).

- **Transport: `delivery_module` (Waku), encrypted with ECIES** — a dedicated topic
  the agent and owner share. The channel is end-to-end confidential: only a client
  holding the matching ECIES key can read it.
- **Why not `chat_module`.** An earlier design routed the owner channel through
  `chat_module`. `chat_module` is **no longer a dependency** — its message dispatch
  is broken upstream — so the owner channel is built directly on `delivery_module`
  plus the agent's own ECIES scheme. This is the honest current state, not a
  temporary shim.
- **Reachability caveat (stated honestly).** Because the channel uses Pilot's own
  ECIES scheme over `delivery_module`, it is readable only by a client that speaks
  the same scheme — in practice the **pilot-ui Basecamp plugin**. A stock,
  third-party chat client cannot open or read the owner channel. This is a
  deliberate confidentiality property, not a limitation we hide: there is no
  plaintext owner channel.
- **Authentication: confidentiality is not authority.** The channel is encrypted to
  the ECIES key the agent's own Agent Card publishes, so anyone who has read the
  card can put a message on it. Who the agent *obeys* is decided by
  `verifyOwnerMessage`: a signed envelope (`{"message", "_logos": {"signing_key",
  "signature", "nonce"}}`) must verify, its signing key must match the one pinned on
  the owner's first signed contact (TOFU, `config.owner.signing_key`), and its nonce
  must strictly increase (`config.owner.last_nonce`). Unsigned text is accepted only
  while **no owner is bound** — the setup window, so a first owner is never locked
  out. Once `owner.npk` is set, or a signed owner has been pinned, unsigned text is
  dropped before it reaches the LLM or any skill. (Fail-open until 2026-09-04; the
  rule is pinned by `tests/test_owner_channel.cpp`.) Before an owner is bound the
  agent's own replies go out unencrypted, which is why binding the owner is part of
  `pilot deploy`.

If the owner cannot be reached, above-threshold actions are **not** executed (see §3).

---

## 3. Spending Threshold

Every value-moving action (wallet sends, and paid outbound A2A asks) passes through
a single spending gate before any funds move.

### Two limits

- `spendLimitPerTx` — maximum LEZ for one transaction (default 100).
- `spendLimitPerPeriod` — maximum cumulative LEZ over `spendPeriodSeconds`
  (default 500 / day).

A spend is **autonomous only if it fits BOTH limits**. The period total is computed
from spends that have actually committed tokens (states `COMPLETED`, `EXECUTING`,
`APPROVED`, `TX_UNKNOWN`); gated states (`CREATED`, `HELD`, `NOTIFIED`) and dead states
(`TX_FAILED`, `REJECTED`, `EXPIRED`) do not count against it.

- **Below both limits → autonomous.** Executed without owner involvement.
- **Above either limit → owner approval.** The request is HELD and the owner is
  prompted on the owner channel.
- **Owner unreachable → no execution.** If the approval notification cannot be
  delivered, the request stays HELD (it is never silently advanced to NOTIFIED or
  executed). The agent retries on recovery and reports honestly; it never moves the
  money to "make progress."

### 9-state FSM (SQLite WAL-persisted)

```
CREATED ─┬─(within limits)──────────────► EXECUTING ─► COMPLETED
         │                                          └► TX_FAILED
         └─(over a limit)─► HELD ─► NOTIFIED ─┬─/approve ─► APPROVED ─► EXECUTING ─► COMPLETED
                                              │                                  └► TX_FAILED
                                              ├─/reject ──► REJECTED
                                              └─timeout ──► EXPIRED
```

- **CREATED** — request recorded, limits being checked.
- **HELD** — over a limit; awaiting successful owner notification.
- **NOTIFIED** — owner notification delivered; awaiting decision.
- **APPROVED** — owner sent `/approve <id>`.
- **EXECUTING** — private transfer submitted on-chain.
- **COMPLETED** — transfer confirmed.
- **TX_FAILED** — transfer failed (funds not moved; reported, not faked).
- **TX_UNKNOWN** — interrupted mid-execution (crash while EXECUTING); outcome
  unverifiable (the wallet exposes no status-by-hash query), still budget-counted; no
  automatic retirement — owner verifies on chain. Surfaced once on the next startup by
  `reconcileExecutingSpends()`.
- **REJECTED** — owner sent `/reject <id>`.
- **EXPIRED** — no owner decision within the timeout.

Every transition is persisted to SQLite (WAL) before action. On crash recovery the
agent re-announces requests stuck in `CREATED` / `HELD` / `NOTIFIED`. Only states
still awaiting the owner can expire; mid-flight and terminal states do not.

---

## 4. A2A Trust Model (asker-pays-doer)

A2A is agent-to-agent JSON-RPC 2.0 over Waku (`delivery_module`), ECIES-encrypted,
in a `_logos` envelope. The inbox is keyed on the agent's ECIES public key. The trust
question is: **what can a stranger make this agent do, and who gets paid?**

### What a stranger CAN make the agent do (autonomous)

- **`agent.ask` — and only `agent.ask`.** This is the single SAFE, autonomous, paid
  service: an LLM query. Pure compute — it touches no files, no identity, no funds,
  no other module. A stranger can submit an `agent.ask` task and the agent will
  answer and (if the asker is paying) be paid, with no owner involvement.
- **Read-only / harmless responses**: serving its own signed Agent Card, status.

### What a stranger CANNOT make the agent do (owner-gated, never autonomous)

All RISKY skill families are routed to the **owner gate** (`input-required` +
owner prompt) regardless of amount, and dispatch nothing for a stranger:

- `storage-*` (touches files), `messaging-*` (sends as the agent),
  `wallet-send` (moves the agent's funds), `program-*`.
- An inbound RISKY task parks at `input-required`; no file is read, no message is
  sent, no token moves. The owner is asked. (Owner *execution* of an approved RISKY
  inbound task is a documented follow-up; the **security guarantee — never
  autonomous for a stranger — holds today**.)

The advertised price/access on the Agent Card is derived from the *same* SAFE-service
catalog that the inbound dispatcher uses, so the card can never advertise a skill as
`autonomous` that the dispatcher would actually owner-gate (`x_access` is
`autonomous` for SAFE services, `owner-approval` for everything else).

### Authenticity: signed cards + signed replies + TOFU pinning

ECIES encryption authenticates **nothing** (anyone can encrypt to a public key), so
authenticity is bound separately by ECDSA-secp256k1 signatures plus
trust-on-first-use pinning:

- **Agent Cards are signed.** A card publishes its skills, pricing, `_logos.payout`
  (shielded LEZ account), `_logos.signing_key`, and per-skill `x_access`. The card is
  signed by its own `signing_key`; `verifyCardStatus()` rejects a card whose
  signature does not match its published `signing_key`.
- **TOFU identity pinning.** The first `signing_key` ever seen for a given `npk` is
  pinned (`pinned_identities: npk → signing_key`). A later card reusing that `npk`
  under a **different** `signing_key` is rejected as `invalid`. This defeats a
  from-scratch forgery that reuses a victim's `npk` with the attacker's own
  `signing_key` + payout. (Honest residual: pinning is TOFU on passive discovery —
  a malicious card seen *first* can squat an `npk`.)
- **Payment requires a SIGNED terminal reply.** The asker pays the doer's
  `_logos.payout` **only** on a terminal `completed` reply whose signature verifies
  against the doer's **pinned** `signing_key` — never against the key carried in the
  reply itself. An unsigned reply, a signature that fails, or no authenticated card
  on file → drop, no settle, no pay.
- **Replay-bound.** The signed reply's embedded task id must equal the id being
  settled, so a genuine `completed` for task A cannot be replayed onto task B's
  public reply topic.
- **At-most-once payment.** Settlement is an atomic `submitted → settling` claim, so
  a repeated `completed` cannot double-pay. Progress replies (`accepted`, `working`,
  `input-required`) never settle; terminal negatives (`failed`, `canceled`,
  `rejected`) are non-settling and retract a payment still pending owner approval.
- **Ambiguous payee → pay nobody.** If two valid cards claim the same `npk` with
  different payouts, the payout resolver refuses (empty) and no payment is made.

### Outbound spend obeys the same gate

When the agent is the *asker* paying a doer, the price runs through the §3 spending
FSM: below both limits it settles autonomously; above either limit it is HELD for
owner approval. The agent never defaults to autonomous execution on its own funds.

---

## 5. Plugin Loader Trust Model

Third parties can add skills without recompiling the core via a Qt plugin
(`PilotPluginInterface`, `pilot_plugin_iface.h`) dropped into an operator directory.
Its trust posture:

- **OFF BY DEFAULT, fail-closed.** Loading does nothing unless the operator sets
  `PILOT_ENABLE_PLUGINS` to `1/true/yes/on` (strict allowlist). Absent or any other
  value → no plugins load.
- **Operator-trusted directory.** Plugins load from `~/.pilot/plugins` (override with
  `PILOT_PLUGINS_DIR`). Placing a file there is an explicit operator act of trust.
- **NOT A SANDBOX — state this plainly.** A loaded plugin runs **in-process with full
  agent privileges**: it can reach the agent's keys and funds. No isolation is
  claimed or provided. The directory is an *operator trust boundary*, not a security
  sandbox. The operator must keep the directory private (file-permission hardening on
  the dir is not enforced by the agent — documented operator responsibility).
- **Failure isolation.** A plugin that fails to load is isolated and logged; one bad
  plugin does not stop the agent. A plugin **cannot shadow a built-in skill**
  (`wallet.send` etc. cannot be overridden by a plugin).
- Assumes Qt6.

---

## 6. Autonomous vs Owner-Approval — at a glance

| Action | Autonomous? |
|--------|-------------|
| Read own wallet balance / history | Yes |
| List / report own status, serve signed Agent Card | Yes |
| Discover peer agents | Yes |
| Answer an `agent.ask` LLM query for a stranger (paid) | Yes (the ONLY autonomous paid A2A service) |
| Spend below per-tx AND per-period limits | Yes |
| Spend above either limit | No — owner approval |
| Spend above limit but owner unreachable | No — held, never executed |
| `storage-*`, `messaging-*`, `wallet-send`, `program-*` requested by a stranger | No — owner-gated (`input-required`), never auto-run |
| Pay a doer on an unsigned / unauthenticated `completed` reply | No — dropped, no pay |
| Load a third-party plugin | No — off unless operator opts in |

---

## 7. Encryption Summary

- **File encryption (AES-256-GCM).** Per-file random 256-bit key + 96-bit IV;
  encrypted before upload, decrypted after download. Key stored in
  `stored_files.file_key_encrypted` as `key:iv:tag`. On share, the file key is
  ECIES-encrypted to the recipient.
- **Message encryption (ECIES, secp256k1).** Ephemeral keypair per message; ECDH →
  SHA-256 KDF → AES-256-GCM; envelope `{ephemeral_pubkey, ciphertext, iv, tag}`.
  Applied to A2A inbox messages, A2A replies, and the owner channel. **Not** applied
  to the discovery topic — Agent Cards are intentionally public (and signed).
- **Signatures (ECDSA-secp256k1).** Agent Cards and terminal A2A replies are signed;
  verified against the doer's TOFU-pinned `signing_key`. Encryption alone never
  authenticates a payee.

---

## 8. Honest Limitations (security-relevant)

- `program.query/call/deploy` is a **verified upstream platform gap** (no program-op
  method exists on the wallet module / wallet-ffi at the pinned revs). The skills
  call the real method and return an honest "unsupported (verified)" error — not a
  Pilot defect, but worth knowing these skills do not transact.
- A2A residual minors: owner-gated RISKY inbound tasks park at `input-required` with
  no owner-execute command yet; `agent.ask` runs the LLM synchronously in the
  delivery event loop (serializes inbound while it runs); TOFU pins on passive
  discovery (a card seen first can squat an `npk`); autonomous pay requires prior
  discovery of the doer's card (else it records accepted-nopay and the doer is
  unpaid); card/reply canonicalization relies on Qt's deterministic JSON
  serialization (cross-implementation verifiers must match it byte-for-byte).
- Plugin loader is operator-trust, not a sandbox; plugins-dir permission hardening is
  the operator's responsibility.
- Build/test status: compiles per repeated static cross-review with unit tests added
  (crypto sign/verify, A2A inbox/outbound, card auth, loader), but **not yet executed
  locally** (dev-box OOM, no Cachix/RISC0 substituter). CI is the source of truth —
  treat correctness claims as **pending CI verification**.
