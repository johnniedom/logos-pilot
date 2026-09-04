# Pilot Agent — Payment Model

## v1: Pay-on-Completion

Decision D7 from [decisions.md](research/decisions.md). The asker pays the doer once, when the
doer's reply reaches the terminal `completed` state; `accepted` / `working` / `input-required`
never settle, and `failed` / `canceled` / `rejected` never pay. (Until 2026-09-04 the Agent
Card advertised `payment_timing: "on-acceptance"` while settlement already worked this way;
the card and the reply envelope now say `on-completion` / `pending-on-completion`.)

### Flow

1. Agent B publishes Agent Card with skill pricing: `"agent-ask": 5 LEZ`
2. Agent A discovers B's card, verifies its signature, pins B's signing key (TOFU)
3. A sends a signed task request to B's inbox (ECIES-encrypted)
4. B accepts the task, starts working — no money moves
5. B completes the task, sends a signed result to A's reply topic
6. A verifies the reply against B's pinned key, then pays B's declared price through its own
   spending FSM (at most once per task; the payout account is the one B's card declares)
7. B sees the payment on chain

### Why Pay-on-Completion (Not Escrow)

- Simpler to implement — no escrow contract needed (and the wallet module exposes no program
  operations yet, see `KNOWN_LIMITATIONS.md` §1)
- The asker never pays for work that did not finish; the doer's risk is bounded by one task
- Matches the trust model of early agent networks (small amounts, known peers)
- The `_logos` envelope includes `payment_timing: "on-completion"` so agents know the model
- Consequence: `agent.cancel` refunds nothing because nothing has been paid before completion

### Pricing

Each skill declares a `priceLez()` value:
- `0` — free (wallet.balance, meta.skills, agent.discover, etc.)
- `1` — cheap (messaging.send)
- `5` — moderate (storage.download, storage.share)
- `10` — standard (storage.upload, program.call)
- `100` — expensive (program.deploy)

Prices are listed in the Agent Card under `_logos.pricing` and in the skill registry via `PilotSkill::priceLez()`.

### Spending Threshold Interaction

When Agent A pays for a task:
- If the payment amount < `spendLimitPerTx`: auto-execute
- If above threshold: create a spend request, notify owner, wait for approval
- Owner sees: "Agent B completed translation task. Cost: 10 LEZ. /approve abc123"

### v2 Path: On-Chain Escrow

For high-value tasks between untrusted agents:
1. A deposits LEZ into an escrow program before task starts
2. B sees the deposit on-chain, begins work
3. On completion, B submits proof → escrow releases funds
4. On timeout/failure, A can reclaim funds

This requires a custom LEZ program (RISC0 guest binary). Documented as a future enhancement — not implemented in v1.

## CU Cost Reference

On LEZ (measured against a local standalone sequencer, v0.1.2) there is **no explicit on-chain CU fee meter** —
the dominant compute cost of every *private* operation is its **zk-proof generation** (RISC0 zkVM).
So the numbers below are proof/compute cost, not a fee. They were measured end-to-end with real
proofs (`RISC0_DEV_MODE=0`) on the reference dev machine on 2026-06-05.

**Reference hardware:** WSL2 / Ubuntu, ~7.6 GB RAM laptop. Proving for these circuits is
**RAM-bound** — on this box the prover swaps heavily, so the wall-clock figures are a worst case.
A machine with adequate RAM (16 GB+) generates the same proofs in *minutes*, not tens of minutes.

| Operation | On-chain proof? | Measured cost (reference HW) | Notes |
|-----------|-----------------|------------------------------|-------|
| `transferPrivate()` — shielded public→private send | **Yes — real STARK** | **~44 min wall / ~139 min CPU** (≈3.2× parallel) | The expensive op; dominates everything. RAM-bound; ≈minutes on 16 GB+. The public testnet mines only real proofs (dev-mode receipts are accepted into the mempool and never mined, measured 2026-08-29). |
| `createAccountPrivate()` | No (local) | instant | Account is created locally; the proof is deferred to its first transfer. |
| pinata claim / public transfer | Public circuit (no privacy proof) | seconds — lands in ~1–2 blocks | Funding path; cheap because no privacy circuit. |
| `transfer_public` — the agent's **public spend rail** (`wallet.send` to `public:<64-hex id>`) | **No client proof** — the client signs, the sequencer proves the block | seconds — mined in the next block (~60 s on the public testnet; measured 2026-09-02, tx `1bbd306b…`) | Spends from the public account the faucet credited. The only rail that settles on the public testnet from an 8 GB box; every spend over it is visible on-chain (no privacy). |
| `account sync-private` | No (decrypt/scan) | **~21 s** | Viewing-key note scan to decode the incoming shielded note. |
| `callProgram()` / `deployProgram()` | Depends on the guest | not exercised in v1 | No custom RISC0 guest ships in v1; a `deployProgram` would add a one-time guest-proving cost. |

**Getting exact, hardware-independent numbers:** wall-clock varies wildly with RAM. The portable CU
measure is the **RISC0 zkVM cycle count** — run the prover with `RISC0_INFO=1` (or read
`session.total_cycles`) to log cycles per segment; sum them for the per-operation cycle cost.

**Takeaway for agent design:** private transfers are the heavy operation, so the pilot batches/holds
rather than proving speculatively, and the spending FSM only triggers a shielded proof once on an
explicitly approved transfer — never on ambiguous input.
