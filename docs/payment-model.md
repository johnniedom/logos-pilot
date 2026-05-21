# Pilot Agent — Payment Model

## v1: Pay-on-Acceptance

Decision D7 from [decisions.md](research/decisions.md).

### Flow

1. Agent B publishes Agent Card with skill pricing: `"storage-upload": 10 LEZ`
2. Agent A discovers B's card, sees the price
3. A sends a task request to B's inbox (ECIES-encrypted)
4. B accepts the task, starts working
5. B completes the task, sends result to A's reply topic
6. A verifies the result, sends LEZ payment to B
7. B confirms payment received

### Why Pay-on-Acceptance (Not Escrow)

- Simpler to implement — no escrow contract needed
- Matches the trust model of early agent networks (small amounts, known peers)
- The `_logos` envelope includes `payment_timing: "on-acceptance"` so agents know the model

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

Measured on LEZ testnet (costs may change during testnet):

| Operation | Estimated CU |
|-----------|-------------|
| `transferPrivate()` | TBD — measure during Phase E |
| `createAccountPrivate()` | TBD |
| `callProgram()` | TBD |
| `deployProgram()` | TBD |

These will be filled in during integration testing with actual on-chain measurements.
