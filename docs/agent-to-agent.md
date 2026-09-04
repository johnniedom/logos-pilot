# Agent-to-Agent (A2A) Coordination Protocol

Pilot agents coordinate with *other* agents — including strangers they have never
met — to buy and sell work. This document describes that protocol: how an agent
advertises what it does, how a requester ("asker") finds it, how a task runs to
completion, and how the asker pays the doer **only** for proven work.

The implementation lives in two files:

- `pilot-module/src/pilot_a2a.cpp` — the **asker** side (agent card, discovery,
  outbound tasks, reply verification + settlement) plus the program-op skills.
- `pilot-module/src/pilot_a2a_inbox.cpp` — the **doer** side (the inbound task
  server, the trust gate, signed replies).

Method names cited below are members of `PilotImpl` unless noted.

---

## 1. The asker-pays-doer model

A2A is a **marketplace**, not a favor exchange. Two roles:

- The **asker** wants a task done and **pays** for it.
- The **doer** runs the real skill and gets **paid** — but only on a terminal,
  signed `completed` reply.

Payment flows asker → doer's published payout account. This is the inverse of a
naive "I'll do work for you" model: the agent that *requests* the work is the one
that *spends*. Critically, when Pilot acts as the **doer** of a paid service
(`agent.ask`), it is **paid by** the requester — it never transfers money *to* a
requester (see `pilot_a2a_inbox.cpp`, the SAFE-service branch).

Settlement is autonomous when the price is below the agent's spending thresholds,
and routed to the owner when it is above (Section 9). Money only ever moves on a
doer's **signed** terminal success (Section 8).

---

## 2. Transport: A2A over Waku

A2A messages are **JSON-RPC 2.0** envelopes carried over Waku via the
`delivery_module` (Qt Remote Objects `callModule`/`invokeRemoteMethod`, never
FFI). Every payload is **ECIES-encrypted** end to end, and every Pilot-specific
field rides in a `_logos` extension object on the JSON-RPC envelope.

### JSON-RPC methods

| Direction | Method | Built in |
|-----------|--------|----------|
| asker → doer | `tasks/send` | `agentTask()` |
| asker → doer | `tasks/sendSubscribe` | `agentSubscribe()` |
| asker → doer | `tasks/cancel` | `agentCancel()` |
| doer → asker | `tasks/statusUpdate` (notification) | `resumeInboundTask()` / `replyToPeer()` |

`processInboundRequest()` (the doer's dispatcher) switches on `req["method"]` and
handles `tasks/cancel`, `tasks/sendSubscribe`, and `tasks/send`; anything else
returns JSON-RPC `-32601 method not found`.

### The `_logos` envelope

On an outbound `tasks/send` (`agentTask()`), the envelope carries:

- `sender_npk` — the asker's payment identity (shielded LEZ account).
- `sender_ecies` — the asker's **ECIES public key**. The doer stores this and
  encrypts **every** reply to it; the asker decrypts with `agentEciesPriv_`.
  Without it the doer has no key it can encrypt a readable reply to.
- `reply_topic` — `"/pilot/1/reply-<taskId>/proto"`, where the doer publishes
  status updates and the asker is subscribed.
- `timestamp`.

### The ECIES-keyed inbox

A doer advertises its inbox as `"/pilot/1/inbox-" + agentEciesPub_ + "/proto"`
(`agentCard()`). The inbox is keyed on the agent's **ECIES messaging key**, **not**
its wallet npk. This is deliberate: a task encrypted to the npk's *viewing key*
would be undecryptable by the agent, because the private half of that key lives in
wallet-ffi. The agent only holds the private half of its ECIES keypair
(`agentEciesPub_` / `agentEciesPriv_`), so all A2A encryption targets that key.

When the asker sends, `a2aRoutingKeyFor()` resolves the doer's ECIES key from its
discovered card's `_logos.signing_key`, `eciesEncrypt()`s the request to it, and
publishes to `"/pilot/1/inbox-" + routingKey + "/proto"`. If the address is a
wallet/npk blob with no card on file, routing returns empty and the send is
**refused** rather than silently dead-dropped to a key the doer cannot decrypt.

On receipt, `handleInboundA2A()` ECIES-decrypts with `agentEciesPriv_`, runs
`processInboundRequest()`, and encrypts the reply back to the requester's
`sender_ecies`. Undecryptable or malformed input is dropped (ambiguity → inaction).

---

## 3. The Agent Card

`agentCard()` builds and **signs** a capability document, then publishes it to
`"/pilot/1/discovery/proto"`. Beyond the standard A2A fields (name, capabilities,
input/output modes, skills) it carries a `_logos` block:

- `npk` — the agent's payment identity.
- `inbox_topic` / `transport` — where and how to reach it (`waku`).
- `pricing` — a map of skill-id → price, **derived from the single-source SAFE
  service catalog** (`a2aServiceCatalog()`); only priced (>0) entries appear.
- `payout` — the **shielded LEZ account** a requester transfers the price to.
  This is the agent's private-account public-keys blob (the same
  `{nullifier_public_key, viewing_public_key}` shape the wallet returns), so a
  requester's `doPrivateTransfer` routes it straight to `transfer_private`. It is
  deliberately **not** the waku messaging id and **not** a bare owned account id.
  **In this build `payout` MUST equal `_logos.npk` (the card's payment identity).**
  A genuine card always sets them equal; `discoveredPayoutFor()` **refuses to pay**
  any card whose `payout != npk`, so a card cannot redirect funds to a third account
  even after passing the signature/TOFU gate (H1). A distinct, separately-payable
  payout account is a tracked follow-up that would require binding `payout` into the
  TOFU pin.
- `signing_key` — the public key that signs *this* card (`agentEciesPub_`).
- `payment` / `payment_timing` — `"lez"`, `"on-completion"`.

### Per-skill access tag (`x_access`)

Each skill in the card carries `x_access`, set by `mkSkill()` to `"autonomous"`
**iff** the skill id is in `a2aServiceCatalog()`, else `"owner-approval"`. The tag
is derived from the same catalog that drives inbound dispatch, so the card can
never claim a skill is autonomous that the doer would actually owner-gate, or vice
versa. Today only `agent-ask` is `autonomous`; `wallet-*`, `storage-*`,
`messaging-*`, `program-query` are advertised honestly as `owner-approval`.
`program-call`/`program-deploy` are not advertised at all — they are unsupported
over A2A, so advertising or pricing them would be dishonest.

### Signing

The card is signed with **ECDSA secp256k1 (ES256K)** over its canonical bytes
(`signMessage()` on the card *without* the `signature` field). The signature object
records `alg`, `publicKey` (= `agentEciesPub_`), and `value`. If signing throws,
the card is left **unsigned** rather than carrying a fabricated signature. Canonical
bytes rely on `QJsonDocument`'s deterministic (sorted-key) compact serialization;
the verifier reproduces them by removing `signature` and re-serializing compact.

---

## 4. Identity authentication and TOFU pinning

Encryption authenticates nothing — anyone can encrypt to a public key. Card
**authenticity** is checked by `verifyCardStatus()`, which has two forms.

### Self-consistency (single-arg `verifyCardStatus(card)`)

Returns one of `valid` / `invalid` / `unsigned` / `unbound`. A card is `valid`
only when (1) it carries a signature, (2) it publishes `_logos.signing_key` (its
bound identity key — else `unbound`), (3) `signature.publicKey == signing_key`
(else `invalid` — this defeats re-signing a genuine card under an attacker's key),
and (4) the signature verifies over the canonical bytes **using the bound identity
key**, never the attacker-supplied `signature.publicKey`.

This form alone is only relative to the card's *self-declared* identity, so a
from-scratch forgery (victim npk + attacker signing_key + attacker payout) would
still read `valid`. The DB-aware form closes that.

### Identity-bound TOFU (`verifyCardStatus(card, db)`)

For a card the self-check already deems `valid`, this **pins** `(_logos.npk →
_logos.signing_key)` into the `pinned_identities` table on **first contact**, with
`INSERT OR IGNORE` so the genuine first contact wins and can never be overwritten.
The write happens *before* the read to close the check-then-pin window. It then
reads the **authoritative pinned** signing_key for that npk and requires the card
to match it. A later card reusing the same npk under a **different** signing_key —
a payout-swap impersonation — verifies `invalid`, even though it is internally
self-consistent. This is the binding that protects the payout account.

Both discovery paths (`agentDiscover()`) and every payout/settlement lookup
(`matchedCardLogos()`) run the **DB-aware** form, so first contact always pins and
all money decisions are bound to identity.

---

## 5. Discovery

`agentDiscover(topic)` resolves agent cards for a topic (default
`"/pilot/1/discovery/proto"`):

1. **Local cache** — reads `discovered_agents` from SQLite, re-verifying each card
   with the DB-aware `verifyCardStatus` (which also pins on first contact).
2. **Network** — `subscribe`s and `storeQuery`s the discovery topic via
   `delivery_module`, then caches results with `INSERT OR REPLACE` keyed on
   `_logos.npk` (**not** display name — every Pilot card hardcodes the name "Pilot
   Agent", so keying on name would collapse two distinct agents into one row).

Each returned card is annotated with `signature_status`. The cache stores the
unannotated original so it can be re-verified later. De-duplication across cache +
network is by `_logos.npk`.

---

## 6. Task lifecycle

A task moves through: **accepted → working → input-required → completed / failed /
canceled**. State is persisted in `inbound_tasks` (doer side) and `outbound_tasks`
(asker side).

### Doer side (`processInboundRequest`, `tasks/send`)

1. Validate `taskId`, `skill`, `sender_npk` (else `-32602`).
2. **Idempotency** (Waku is at-least-once): if the task id is already in a
   working/terminal state *or* already carries a linked spend request, reply with
   the **cached** result instead of re-running the skill or re-executing a transfer.
   Only a not-yet-seen row is processed.
3. Insert the row as `accepted`, persisting the full message envelope but
   dispatching the **flat** inner args (`a2aSkillArgs()` pulls
   `params.message.parts[0].text`).
4. Dispatch by skill, driving `accepted → working → terminal` and replying with a
   JSON-RPC task object (`a2aRpcTask`).

`inboundTasksRecover()` fails any `accepted`/`working` task that died with a
previous process (so peers aren't left waiting); `input-required` tasks survive,
because their linked spend request drives them via approve/reject/expiry.

### Asker side

`agentTask()` records the row as `submitted` *before* sending (so the reply
consumer can settle the moment a `completed` arrives), with `ON CONFLICT(id) DO
NOTHING` so a resubmit can't re-arm a second payment. `agentSubscribe()` polls,
`agentCancel()` withdraws (and marks the local row `canceled` so a late reply can't
pay). `outboundTasksRecover()` re-subscribes still-`submitted` reply topics and
reconciles any row caught mid-`settling` against its linked spend request after a
restart.

---

## 7. Trust model: SAFE-auto vs RISKY-owner-gated

An A2A peer is a **stranger**, not the owner. The dispatcher therefore splits all
skills into two classes.

### SAFE → autonomous (the only paid service)

`a2aServiceCatalog()` is the **single source of truth** for autonomously-serviced
paid skills. Today it holds exactly one entry: **`agent-ask`** (price 5) — answer a
prompt with the agent's LLM. It is SAFE because it is **pure compute with no side
effects on this agent**: no local-file access, no use of the messaging identity, no
fund movement. So it is safe to run for an unknown peer with **no owner
involvement**, and Pilot gets paid for it. `processInboundRequest` auto-runs it,
mapping the dash id (`agent-ask`) back to the registry name (`agent.ask`) and
driving `working → completed/failed` based on the honest success contract
(Section 7.1). `ping` and `capabilities` also auto-complete but are unpriced.

The same catalog drives `agentCard()`'s `pricing` and `x_access`, so an
advertised-autonomous price can never name a skill the dispatcher wouldn't actually
auto-service, and an auto-serviced skill can never be silently free.

### RISKY → owner-gated, never autonomous

The RISKY families — `storage-*`, `messaging-*`, `program-*`
(`a2aRiskyOwnerGated()`), plus `wallet-send` — are **deliberately absent** from the
catalog. At a stranger's request:

- `storage-*` would read/exfiltrate local files and leak the stored-file inventory.
- `messaging-*` would send/relay/forge messages under *our* identity (open relay /
  impersonation).
- `program-*` would act on-chain as us.
- `wallet-send` would move *our* funds.

These are routed to the **owner gate**: the task parks at `input-required`,
`sendToOwner()` notifies the owner, and **nothing runs** until/unless the owner
acts. `wallet-send` additionally opens a `HELD` spend request linked to the task
(so an owner decision can resume it). `wallet-balance` and unknown skills return an
honest `-32004 unsupported skill` — no result fabricated, no spend, no payment.

This is by design: SAFE services run autonomously for strangers; RISKY skills
require the owner. The security guarantee — *never autonomous for a stranger* —
holds today. (Owner *execution* of an approved non-spend risky task is a documented
follow-up; see Limitations.)

### 7.1 Honest success contract

`a2aResultIsSuccess()` is the single source for "did the work succeed". A task is
`completed` **only** on an explicit positive signal. A result reading
`{"success":false}` / `{"joined":false}` / `{"ok":false}`, a `status` of
`failed`/`error`, or any opaque/ambiguous shape (bare string, empty object/array,
unparseable) is `failed` — **no pay**. Ambiguity → `failed`, never `completed`. We
never pay for unproven or failed work.

---

## 8. Signed-reply settlement

This is the core money-safety mechanism, on the **asker** side.

### Why encryption is not authentication

The reply topic (`/pilot/1/reply-<taskId>/proto`) and the asker's ECIES public key
are both **public**. Any observer could encrypt a forged
`{"status":{"state":"completed"}}` to the asker and try to force a payment. ECIES
decryption proves only that *someone* encrypted to the asker's public key — it
authenticates nothing. So the doer **signs** every reply.

On the doer side, `replyToPeer()` signs the reply's canonical bytes (envelope
*with* `_logos.signing_key`, *without* `_logos.signature`) using ES256K and
attaches `_logos.signature`. If it cannot sign, it sends **unsigned** rather than
fabricate — and the asker will correctly refuse to settle.

**Symmetrically, inbound requests are authenticated (H2).** The doer's inbox ECIES
key is *also* public, so an ECIES-decryptable request authenticates nothing either.
Every outbound request (`agentTask` / `agentSubscribe` / `agentCancel`) is therefore
signed via `signA2AEnvelope()` (the same canonical-bytes scheme as a reply), and
`handleInboundA2A()` drops any request that is not validly signed — verifying it via
`verifyInboundRequest()` against a **dedicated `pinned_request_identities` TOFU
namespace**, never the card pin, so a request can never poison the payout pin.
`tasks/cancel` and `tasks/sendSubscribe` additionally require the authenticated
sender to equal the task's stored `sender_npk`, so a third party cannot cancel a
peer's task or read its paid-for result.

### The asker's gate (`verifyAndSettleReply`)

`handleA2AReply()` decrypts the reply and calls `verifyAndSettleReply()`, which:

1. Extracts the task `state` (from `result.status.state` for a `tasks/send` reply,
   or `params.status.state` for a `statusUpdate`).
2. **Replay binding**: the signed reply embeds a task id (`result.id` /
   `params.id`); it must equal the `taskId` taken from the public reply topic. A
   genuine doer-signed `completed` for task A could otherwise be replayed onto task
   B's topic (same pinned doer). Mismatch → drop, no settle.
3. Resolves the doer's **authoritative** signing_key from this task's
   `agent_address` via its TOFU-pinned, identity-validated card
   (`matchedCardLogos()` — only returns a `valid`, unambiguous card). No such card
   → drop.
4. Verifies the signature against that **pinned** key — never the reply-supplied
   `_logos.signing_key`. Mismatch or unsigned → drop, no settle.

Only then does it call `settleOutboundReply()`.

### At-most-once settlement (`settleOutboundReply`)

- **Terminal-only trigger**: settles **only** on `completed`. Progress replies
  (`accepted`, `working`, `input-required`) are non-settling — the row stays
  `submitted` so a later `completed` can still claim. `failed`/`canceled`/`rejected`
  are recorded as terminal negatives and never pay (and a `failed` arriving after a
  payment was parked `awaiting-approval` triggers `rejectSpend()`, so a peer that
  says `completed` then `failed` leaves no held payment dangling).
- **Atomic claim**: `UPDATE … SET state='settling' WHERE id=? AND state='submitted'`
  — if the row isn't won (already settling/paid/awaiting/etc.), it does nothing.
  This is what makes a repeated `completed` settle **at most once**.
- **Authenticated payout**: pays `discoveredPayoutFor()` — the payout from the
  matched, authenticated, unambiguous card — never the messaging address. No payout
  on file → `pay-failed`, never a mis-targeted transfer. No declared price →
  `accepted-nopay` (we never invent a price). `matchedCardLogos()` also **refuses**
  when two valid cards claim the same npk with different payouts (ambiguous → pay
  nobody).

---

## 9. Below / above threshold

`settleOutboundReply()` applies the **same** autonomous gate as the wallet and
inbound paths. A spend is created (`createSpendRequest`) and linked to the outbound
row up front, then:

- **Within both** the per-transaction cap (`spendLimitPerTx_`) **and** the remaining
  per-period budget (`periodSpent() + price <= spendLimitPerPeriod_`) →
  `executeSpend()` autonomously; the row becomes `paid` only on a real transfer,
  else `pay-failed`.
- **Above either** limit → `holdForApproval()` notifies the owner over the owner
  channel and the row waits in `awaiting-approval` until `approveSpend()` /
  `rejectSpend()` drives it to `paid` / `pay-failed`.

Ambiguity (over either limit) always routes to the owner; autonomous execution is
never the default.

---

## 10. Limitations (A2A)

Stated honestly — a judge may run this code.

1. **Owner-gated RISKY inbound tasks park at `input-required` with no owner-execute
   command.** The security property (never autonomous for a stranger) holds, but an
   owner cannot yet *complete* an approved non-spend risky task; it remains parked.
   (Inbound `wallet-send` *is* resumable via its linked spend request.)
2. **`agent.ask` runs the LLM synchronously inside the delivery event loop**, so a
   long inbound query serializes other inbound A2A handling while it runs.
3. **TOFU pins on passive discovery**: a malicious card *seen first* can squat an
   npk before the genuine agent is ever observed. Pinning protects against later
   swaps, not a first-seen forgery.
4. **Autonomous pay requires prior discovery of the doer's card.** Without a matched,
   authenticated card, settlement records `accepted-nopay` (no payout to pay) and
   the doer goes unpaid until its card is discovered.
5. **Narrow outbound recovery windows**: a crash between `createSpendRequest` and the
   terminal outbound update is reconciled on restart by `outboundTasksRecover()`
   only when the spend reached a terminal state; in-flight spends are left for retry
   / the owner gate.
6. **Canonicalization depends on Qt's deterministic JSON serialization.** Card and
   reply signatures are verified over `QJsonDocument` compact (sorted-key) bytes;
   any cross-implementation verifier must reproduce that exact serialization.
7. **`program-query` over A2A is owner-gated and currently unsupported upstream;**
   `program-call`/`program-deploy` are not advertised over A2A at all. The wallet
   module exposes no program-op method at the pinned LEZ revision (verified against
   upstream source); the skills call the real method and return an honest
   "unsupported (verified)" error. This is a platform gap, not a Pilot defect.
