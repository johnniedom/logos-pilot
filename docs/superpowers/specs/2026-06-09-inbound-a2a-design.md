# Inbound A2A Task Server — Design

**Date:** 2026-06-09
**Status:** Approved by owner (with approve-then-resume loop in scope)
**Fixes:** Punch-list #1 — the pilot drops every message not on the owner channel
(`pilot_impl.cpp` messageReceived: `if (topic != ownerChannelId_) return;`), so it can
send tasks to other agents but cannot receive any. No task lifecycle states exist.

## Goal

Make the pilot a real A2A *server*: other agents can send it JSON-RPC 2.0 tasks over
Waku, watch them move through the standard lifecycle, and receive results — while the
owner's sovereignty rule holds: **nothing that costs money or uses owner resources runs
without owner approval. Ambiguity defaults to inaction.**

## Decisions (made with owner)

1. **Policy: safe-auto / costly-approval.** Free, read-only skills auto-complete.
   Anything that spends LEZ or uses storage/programs requires owner approval first.
2. **Safe skills: `ping` and `capabilities` only.** `ping` echoes liveness;
   `capabilities` returns the existing Agent Card (already public via discovery).
   Balance is NOT exposed to peers (privacy).
3. **Approve-then-resume loop IN scope.** When the owner approves a held peer task,
   the agent executes it and pushes the result to the peer's reply/task topic.
   Reject and expiry also notify the peer (`failed` with reason).
4. **Payment collection deferred.** The Agent Card keeps advertising pricing, but this
   iteration does not verify or collect LEZ payment for tasks. Follow-up feature.
5. **Persistence:** new `inbound_tasks` SQLite table in the existing pilot.db.

## Task lifecycle (A2A standard states)

```
submitted (peer sends tasks/send)
   └─> accepted ── safe skill ──> working ──> completed | failed
              └── costly skill ─> input-required (owner approval raised)
                                      ├─ owner approves ─> working ─> completed | failed
                                      ├─ owner rejects ──> failed ("owner rejected")
                                      └─ expires (60 min) ─> failed ("approval expired")
canceled: peer may send tasks/cancel at any non-terminal state
```

State changes are written to `inbound_tasks` and (per A2A `stateTransitionHistory`
capability) pushed to the task topic `/pilot/1/task-<taskId>/proto` so subscribed
peers see progress.

## Message flow

**Inbound:** at delivery-module init, additionally subscribe to our own inbox
`/pilot/1/inbox-<agentNpk>/proto` (the topic our Agent Card already advertises).
The existing `messageReceived` handler routes by topic:
- owner channel → existing owner path (unchanged)
- our inbox → `handleInboundA2A(payload)`

**handleInboundA2A:**
1. ECIES-decrypt with `agentEciesPriv_` (peers encrypt to our published key, same as
   our outbound `agentTask` does toward them). Undecryptable/garbage → drop silently
   (inaction on ambiguity).
2. Parse JSON-RPC 2.0. Supported methods: `tasks/send`, `tasks/cancel`,
   `tasks/sendSubscribe` (re-send current status). Unknown method → JSON-RPC error
   `-32601` to the reply topic.
3. `tasks/send`: extract task id, skill (params.metadata.skill), params, and
   `_logos.sender_npk` + `_logos.reply_topic`. Insert into `inbound_tasks`
   (state=accepted). Then dispatch:
   - `ping` → working → completed; result `{pong: true, agent: <npk>, ts}`.
   - `capabilities` → working → completed; result = agentCard() JSON.
   - costly/known skill (wallet-send, storage-*, messaging-send, program-*) →
     `createSpendRequest(...)` — amount = the requested transfer amount for
     wallet-send, else the card price for the skill (pricing[skill]); reason =
     "A2A task <id>: <skill> from <sender_npk>". Mark HELD/NOTIFIED via existing FSM,
     sendToOwner notification, task state=input-required, reply to peer with
     input-required + "awaiting owner approval".
   - unknown skill → failed; JSON-RPC error to reply topic.
4. Every state change is published (encrypted to sender's npk) to the peer's
   `reply_topic` (for the requester) and the task topic (for subscribers).

**Resume loop:** `approveSpend()`/`rejectSpend()`/`expireStaleSpends()` gain a hook:
after handling a spend request, look up the linked inbound task (new column
`spend_request_id` in `inbound_tasks`). On approve → execute the skill via the
existing skill dispatch, state working → completed/failed, push result to peer.
On reject/expiry → state failed, push failure to peer.

## Schema

```sql
CREATE TABLE IF NOT EXISTS inbound_tasks (
    id TEXT PRIMARY KEY,              -- task id from the peer's JSON-RPC
    sender_npk TEXT NOT NULL,
    reply_topic TEXT NOT NULL,
    skill TEXT NOT NULL,
    params_json TEXT NOT NULL,
    state TEXT NOT NULL DEFAULT 'accepted',
    spend_request_id TEXT,            -- set when state=input-required
    result_json TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
```

## Components

- `src/pilot_a2a_inbox.cpp` (new): `handleInboundA2A()`, lifecycle helpers
  (`inboundTaskSetState`, `replyToPeer`), skill dispatch for ping/capabilities,
  resume-loop executor.
- `pilot_impl.cpp`: inbox subscription + topic routing in messageReceived;
  `inbound_tasks` table in initDatabase.
- `pilot_spending.cpp`: approve/reject/expire hooks call the resume executor.
- `pilot_impl.h`: declarations.

## Error handling

- Decrypt/parse failures: drop (no reply — can't trust the envelope).
- Valid envelope, bad request: JSON-RPC error object to reply topic.
- replyToPeer uses the same honest retrying pattern as deliverToOwner (3 attempts,
  parse result, never claim success on failure); a failed peer reply is logged and
  the task still records its true state locally.
- Restart recovery: tasks in `accepted`/`working` at boot → failed ("agent
  restarted"); `input-required` tasks survive (their spend request drives them).

## Testing (TDD)

1. **Standalone red/green** (system g++ + real SQLite, like #5): lifecycle state
   machine + JSON-RPC parse/dispatch as pure functions — valid task, unknown method,
   unknown skill, cancel, approve-resume, reject, expiry.
2. **Harness unit tests** (`tests/test_a2a_inbox.cpp`): same cases via PilotImpl with
   in-repo framework once the nix build is healthy.
3. **Two-agent E2E** (existing Agent-A host + Agent-B docker + shared nwaku):
   B sends `ping` to A → completed result arrives at B. B sends `wallet-send` →
   A's owner sees approval prompt; approve → B receives completed; reject → failed.

## Out of scope (follow-ups)

- Payment verification/collection for tasks (pricing stays advertisement-only).
- Push notifications/streaming beyond topic publishes.
- Rate limiting / peer allowlists (single-peer demo scale).
