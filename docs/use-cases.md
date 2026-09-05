# Use cases on the public LEZ testnet

Three of the prize's illustrative use cases, each run end to end against the public testnet
(`https://testnet.lez.logos.co`) from a clean clone, with every step asserted and the result
read back from the chain or from the receiving agent, never from the sender's say-so. Each
section gives the goal, the exact command, what to expect, and where the evidence is.

Everything here runs on GitHub-hosted runners (`.github/workflows/ci.yml`,
`testnet-agents.yml`, `testnet-use-cases.yml`) and keeps its logs as artifacts. The same
commands run on any Linux box with Nix, Python 3, curl and Docker (for the local Waku relay).

## 1. Personal file vault

**Goal.** The agent keeps a file nobody but its owner can read: encrypted before it leaves the
agent, stored on Logos Storage, retrievable and decryptable later, and shareable with one
chosen identity.

**Command.**

```bash
./demo.sh                                  # step 6: upload -> download -> byte compare
agents/deploy-agent.sh --role storage      # plus: share the key with a second identity, which
                                           # fetches the CID from the first agent's node and decrypts it
```

**What happens.** `storage.upload` encrypts the file with a fresh AES-256-GCM key, stores the
ciphertext on the agent's storage node and records the CID with the key in the agent's own
database. `storage.download` fetches the CID (locally, or over the storage network) and
decrypts it. `storage.share` sends the key to the second identity sealed to that identity's
encryption key; the receiver records it and can then download and decrypt the same CID.

**Expect.** The demo prints `cid zDv... uploaded encrypted and downloaded back byte-identical`;
the storage role ends with `STORAGE AGENT PASSED ... B fetched it from A's node and decrypted it
byte-identical`.

**Evidence.** Every CI run of `demo.sh` (artifact `e2e-demo-logs`). Cross-identity sharing:
`testnet-agents.yml` runs 33923468614 and 33933531544 — agent B's storage node log shows its
block-exchange stream to agent A's peer id, and the artifact holds A's original next to B's
fetched copy. Identities: `evidence/testnet-agents.tsv` (roles `storage`).

## 2. On-chain event alerter

**Goal.** The agent watches an account on the chain and tells its owner, over Logos Messaging,
the moment it changes.

**Command.**

```bash
agents/deploy-agent.sh --role alerter
```

**What happens.** Two agents fund themselves from the faucet. Agent A reads agent B's public
account through the wallet module (`chainAccount`: balance and nonce, a chain read, never a
spend). Agent B then pays 1 LEZ to a project account through its own spending FSM — the event.
A polls the account until the balance changes, then sends B a direct message: `ALERT: account
<id> balance 150 -> 149 (nonce 1 -> 2); tx <hash> in block <n>`. B reads it from its inbox.

**Expect.** `ALERTER USE CASE PASSED ... A alerted B over Logos Messaging and B read the alert`.

**Evidence.** `testnet-use-cases.yml`, job `alerter`: the `EVIDENCE` lines name the watched
account, the triggering transaction (also a row in `evidence/testnet-transactions.tsv`, verified
by `evidence/verify-testnet.sh`) and the alert as received by B (`agent-b-inbox.json` in the
artifact). Run ids and identities: `evidence/testnet-agents.tsv` (role `alerter`).

## 3. Paid skill marketplace

**Goal.** One agent sells a skill; another discovers it, buys it, and pays the declared price
on completion, in shielded LEZ, with nobody in between.

**Command.**

```bash
DEEPSEEK_API_KEY=... agents/deploy-agent.sh --role marketplace     # needs r0vm 3.0.5 and ~16 GB RAM
```

**What happens.** Agent A (the buyer) funds itself and moves 100 LEZ into its private account
with a real RISC0 proof (`RISC0_DEV_MODE=0`; the public testnet mines only real proofs). Agent B
(the seller) gets a language model, opens for hire and publishes its signed Agent Card, whose
one autonomously served skill is `agent.ask` at a declared price. A discovers the card on the
discovery topic, submits an `agent-ask` task over JSON-RPC 2.0 sealed to B's encryption key, B
answers, and A's settlement pays B's declared price to B's private keys — a second real proof.
The payment is a row in A's spending ledger with a transaction hash; the chain confirms it, and
A's private balance drops by exactly the price.

**Expect.** `MARKETPLACE USE CASE PASSED ... paid B's declared 5 LEZ over the private rail with
a real proof — tx <hash> in block <n>`. About two hours on a 16 GB runner, almost all of it
proving.

**Evidence.** `testnet-use-cases.yml`, job `marketplace`: the `EVIDENCE` lines (fund, discover,
pay) and the daemon logs; the payment transaction in `evidence/testnet-transactions.tsv`,
verified by `evidence/verify-testnet.sh`. Run ids and identities: `evidence/testnet-agents.tsv`
(role `marketplace`).

## Status

| Use case | First green run on the public testnet |
|----------|----------------------------------------|
| Personal file vault | every CI demo; cross-identity sharing 2026-09-04, run 33923468614 |
| On-chain event alerter | 2026-09-05, run 33939827003: A watched `Eozv…` (150, nonce 1), B's spend `772889d0…` in block 38221 moved it to 149, A alerted B, B read the alert |
| Paid skill marketplace | pending — the job exists (needs the `DEEPSEEK_API_KEY` secret), see the evidence file for the run once it lands |
