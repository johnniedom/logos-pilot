# Known Limitations

This is the complete, honest list of what Pilot (LP-0008) does **not** do, or does
only partially, as of this submission. It is written for a judge who may read the
source and run the code: every gap below is real, and overclaiming would be worse
than documenting it plainly. Where a limitation is an upstream platform gap rather
than a Pilot defect, that is stated and the evidence is given. Where there is a
clear path to closing it, that path is named.

Nothing here is softened. If a thing is unverified, it says so.

---

## 1. Program operations (`program.query` / `program.call` / `program.deploy`) — upstream platform gap

**What it is.** The three `program-*` skills are wired through the full agent
pipeline — spending-threshold check, owner-approval routing, the A2A trust gate —
but they cannot actually query, call, or deploy a LEZ program. Each one calls the
real underlying wallet method and returns an honest `unsupported (verified)` error.
`program.call`/`program.deploy` are not advertised over A2A at all; `program.query`
over A2A is owner-gated and likewise unsupported.

**Why.** This is a **verified upstream gap, not a Pilot defect**. A direct source
audit of the two pinned dependency revisions —
`logos-execution-zone-module @ 5d42559` and `lssa @ cf3639d` — found **no
program-operation method of any name** on the wallet module or on `wallet-ffi`.
Program deployment on LEZ is performed by submitting a direct sequencer transaction
(`NSSATransaction`) that the wallet module does not expose to its callers. Pilot
cannot invent an API the platform does not provide, so it calls the real method and
surfaces the platform's absence honestly rather than faking a success.

**What would close it.** Upstream `logos-execution-zone-module` / `wallet-ffi`
exposing a program-op entry point (query/call/deploy), or a sanctioned way to
submit the `NSSATransaction` deploy path through the module boundary. When that
lands, the skills already in place activate with no Pilot-side redesign — only the
inner call changes.

---

## 2. A2A protocol — residual minor gaps

The agent-to-agent marketplace (asker-pays-doer, JSON-RPC 2.0 over Waku, ECIES,
signed agent cards, TOFU identity pinning) is implemented and exercised by unit
tests. The following residual minors remain. They are documented in full in
[`docs/agent-to-agent.md`](docs/agent-to-agent.md) (§ "Residual limitations"); the
complete list is reproduced here so nothing is hidden.

1. **Owner-gated RISKY inbound tasks have no owner-execute command.** A risky
   inbound task (`storage-*`, `messaging-*`, `program-*`, and any non-spend risky
   skill) correctly refuses to run autonomously for a stranger and parks at
   `input-required`. But there is currently **no owner command to complete it**, so
   it stays parked. The security property (never autonomous for a stranger) holds;
   the convenience of finishing it does not. Exception: inbound `wallet-send` *is*
   resumable, because it is linked to its spend request.

2. **`agent.ask` runs the LLM synchronously inside the delivery event loop.** A
   long inbound query serializes other inbound A2A handling while it runs. Correct,
   but not concurrent.

3. **TOFU pins identity on passive discovery.** A malicious card *seen first* can
   squat an `npk` before the genuine agent is ever observed. Pinning
   (`pinned_identities: npk → signing_key`) protects against a *later* key swap on a
   pinned npk, not against a *first-seen* forgery.

4. **Autonomous pay requires prior discovery of the doer's card.** If the asker has
   no matched, authenticated card for the doer, settlement records `accepted-nopay`
   (there is no payout account to pay) and the doer goes unpaid until its card is
   discovered.

5. **Narrow outbound-row recovery windows.** A crash between `createSpendRequest`
   and the terminal outbound update is reconciled on restart by
   `outboundTasksRecover()` only when the spend reached a terminal state; in-flight
   spends are left for retry / the owner gate.

6. **Canonicalization relies on Qt's deterministic JSON serialization.** Agent-card
   and reply signatures are verified over `QJsonDocument` compact (sorted-key)
   bytes. Any cross-implementation verifier (a non-Qt agent) must reproduce that
   exact serialization or signature verification will fail. ECIES encryption alone
   authenticates nothing — authenticity comes only from the ECDSA-secp256k1
   signature checked against the doer's *pinned* signing key.

7. **`agent.cancel` has no refund path — by design, because there is nothing to
   refund.** The asker pays the doer only on the doer's terminal `completed` reply
   (`settleOutboundReply`); `accepted` / `working` / `input-required` never settle
   and `failed` / `canceled` / `rejected` never pay. So a task cancelled before
   completion has moved no money. The other side of that design: a task cannot
   be cancelled once it is `completed`, and its payment is final. An escrow-style
   refund (pay on acceptance, claw back on cancel) is the future model described in
   [`docs/payment-model.md`](docs/payment-model.md) and needs an on-chain program.

**What would close them.** (1) an `owner-execute`/`owner-approve-task` command that
resumes a parked risky inbound task; (2) offloading `agent.ask` LLM execution off
the delivery thread; (3) an out-of-band identity attestation to defeat first-seen
squatting; (4) on-demand card fetch at settlement time; (5) finer-grained outbound
state journaling; (6) a documented canonical-JSON spec independent of Qt; (7) an
escrow program on LEZ, if pay-before-completion is ever wanted.

---

## 3. Plugin loader — operator-trust boundary, **not** a sandbox

**What it is.** Third parties can add skills without recompiling the core via a Qt
plugin (`PilotPluginInterface`, `pilot_plugin_iface.h`) dropped into an operator
directory. It is **off by default** and only loads when the operator explicitly
sets `PILOT_ENABLE_PLUGINS` to `1/true/yes/on` (strict allowlist, fail-closed; any
other value, or unset, loads nothing). The scanned directory is `~/.pilot/plugins`,
overridable with `PILOT_PLUGINS_DIR`. Load failures are isolated and logged, and a
plugin **cannot shadow a built-in skill**.

**Why it is a limitation.** The loader is an **operator-trust boundary, not a
security sandbox**. A loaded plugin runs **in-process with full agent privileges** —
it can reach the wallet keys and the money. Placing a file in the plugins directory
is an explicit, irreversible act of trust by the operator. In addition:

- **Plugins-dir file-permission hardening is not enforced.** The agent does not
  verify that the directory is private; the operator **must** keep it private (e.g.
  `chmod 700`) so an attacker cannot drop a `.so` there.
- **Assumes Qt6.** Plugins must be built against the same Qt6 ABI as the host.

**What would close it.** True isolation would require an out-of-process / sandboxed
plugin host (separate process, capability-scoped IPC, seccomp/namespaces) rather
than in-process `QPluginLoader`. Directory-permission checks could be enforced at
load time. Until then the honest framing is: **this is an operator convenience for
trusted code, not a defense against untrusted code.**

---

## 4. Build & test — unit suite green; the end-to-end CI job settles a real spend on the public testnet

**What it is.** The module **builds** and the full **unit suite (188 tests) passes**.
The end-to-end CI job used to boot a Docker devnet sequencer on port 8080 that the
wallet never talked to (the module defaults to `:3040`, `PILOT_SEQUENCER_ADDR` was
never set, `initialize` was never called, and the only wallet line was `|| true`) —
so its "E2E OK" proved that four modules load and echo answers, nothing about money.
On 2026-09-04 it was replaced by a job that runs the same `./demo.sh` a reviewer runs
from a clean clone, against the **public testnet**, with every step asserted: load,
23 skills, echo, self-funding from the faucet verified on chain, a spend through the
spending FSM verified by `getTransaction` and the recipient's balance, and the vault
round-trip. First green run 2026-09-04 (run 33880060967, 21 minutes on a hosted runner
that had never seen the chain): faucet claim into `6U4d…` verified at 150, spend
tx `1398d754…c273` mined in block 37450, sender 150 → 149, recipient 170 → 171, all
read back with `getAccount` / `getTransaction`. What remains outside this job is the
**shielded leg** (a real RISC0 proof, ~16 GB of RAM — `real-proof.yml` runs it by
hand on a 16 GB runner) and the **live two-agent A2A round trip**.

**Why CI rather than local.** A local build is infeasible on the development box:
building `wallet-ffi` + RISC0 OOM-crashes the WSL VM, with no local Cachix / RISC0
substituter. CI (with the Cachix cache `logos-pilot-johnniedom` and a disk-freed
runner) is the source of truth — and is green.

**What is measured outside CI.** The live two-agent round trip
(`test-two-agents-docker.sh`, host agent + Docker agent, local sequencer in dev
mode) does assert money on the chain: after the paid `agent-ask` task settles it
polls the payer's synced balance for up to four block intervals and requires the
drop to equal the declared price (measured 2026-08-26: 100 → 99 for the direct
transfer, then → 94 for the 5-LEZ task payment, both in the next block). It runs
on the development box, not in CI.

**What would close the remainder.** A run that drives real funding/spending with
`RISC0_DEV_MODE=0` against a sequencer, and the two-agent settlement above in CI,
to confirm the on-chain behavior the unit/E2E suites do not assert.

---

## 5. Testnet evidence & demo video — public-chain spending works; the evidence set and the video are not yet in the repo

Status of the submission criteria:

- **F9 — ≥ 3 use cases demoed end-to-end on testnet:** not yet met (the recorded
  runs below are single operations, not the three use cases).
- **F10 — three agents deployed on the public testnet, one per skill category
  (Storage, Messaging, Blockchain), each with reproducible deployment steps and
  on-chain evidence:** not yet met. The Blockchain agent's spend path is live (see
  below); the three deployments and their evidence files are not yet committed.
  (An earlier revision of this document quoted the pre-2026-05-25 wording, "≥ 5
  third-party deployments"; the criterion has been the three-agents-on-testnet
  form since then.)
- **Supportability #1 — public testnet deployment:** partially met — one agent has
  registered, been funded, and spent on the public testnet from this module; the
  reproducible steps are not yet written up.

**What is proven on the public testnet** (`https://testnet.lez.logos.co`, not
auth-gated; every item below is a chain read via `getAccount` / `getTransaction`,
not a module log line):

- 2026-08-27: `lez_core` re-pinned to the module revision tracking **LEZ v0.2.2**
  (branch `feat/lez-v0.2.2`). 2026-08-29: all five program image IDs the testnet's
  `getProgramIds` returns are **byte-identical** to the pinned build's. The version
  gap an earlier revision of this section described is closed.
- 2026-08-29: `register_public_account` and the piñata `claim_pinata` issued by
  Pilot's own funding code were **mined** — the account it created shows
  `program_owner = authenticated_transfer`, balance 150, nonce 1.
- 2026-09-02: a **public transfer** between two accounts this wallet owns was
  **mined in one block** (tx `1bbd306b…`; sender 150 → 140, receiver 150 → 160,
  both nonces 1 → 2). A public transfer is signed by the client and proven by the
  sequencer, so it needs **no client-side RISC0 proof**. `wallet.send` exposes
  this rail as the `public:<64-hex account id>` recipient form, spending from the
  public account the faucet credited.

- 2026-09-04: the **shielded step with a real proof** (`RISC0_DEV_MODE=0`, r0vm
  3.0.5) landed on the public testnet from `.github/workflows/real-proof.yml` on a
  GitHub-hosted 4 vCPU / 16 GB runner (run 33880084026): the account the agent
  claimed, `F1MB…`, went 150 → **50, nonce 2** — the module's 100-LEZ
  `transfer_shielded_owned` into the agent's private account was mined. The
  testnet does not mine dev-mode receipts (2026-08-29: accepted into the mempool,
  never mined), so this is a real STARK accepted on chain. Funding end to end
  (cold sync, claim, proof) took about 50 minutes on that runner.

**What is not proven, and why.**

- The shielded proof does **not** complete on the 8 GB development laptop: the
  prover holds ~4.5 GB inside a 5 GB WSL VM and runs swap-bound for hours (six
  attempts, 2026-08-29 to 2026-09-04; the last proved for four hours of VM time
  before the module's own four-hour ceiling on the wallet call expired). It is a
  memory problem, not a protocol one — the same code path succeeded on 16 GB in
  under an hour. Private spends (`transfer_private*`) carry the same proof cost.
- The `RISC0_DEV_MODE=0` real-proof scripts **are** committed
  (`run-sequencer-realproof.sh`, `demo-realproof.sh`, both with a `REHEARSE=1`
  dev-mode dry-run switch), but the end-to-end **demo video is not yet recorded /
  committed.**

**What would close it.** Commit the three category agents' deployment steps and
their recorded transaction hashes with a re-verification script that reads them
back from the chain; run the three use cases against those agents; finish one
shielded funding proof on a ≥ 16 GB machine (a public CI runner qualifies) so the
private rail is demonstrated too; record the narrated video.

---

## 6. A wallet transfer does not survive an unreachable sequencer — upstream fragility

**What it is.** If the sequencer stops answering on `:3040` while a transfer is
being built, the agent does not report a failed transfer and carry on — the wallet
module **process dies**. Every subsequent wallet call then returns
`{"code":"RPC_FAILED"}` until the daemon is restarted. The spending FSM behaves
correctly throughout (the request is marked failed, **no tokens move**), but the
agent is left without a wallet until an operator restarts it.

**Why.** Upstream wallet code unwraps the transport result rather than propagating
it: `wallet/src/privacy_preserving_tx.rs:223` — `called 'Result::unwrap()' on an
'Err' value: client error (Connect) ... Connection refused (os error 111)` —
which panics inside the module process and aborts it. Observed live on
2026-07-25 in `~/.pilot/daemon.log`; the same shape as the module abort seen on
2026-07-14. Pilot cannot catch a panic that aborts the process it lives in.

**What Pilot does about it.** It cannot prevent the abort, so it makes it legible
and recoverable: the CLI translates `RPC_FAILED` into which module died, that
nothing was sent, the likely cause and the restart step, instead of showing a raw
error that reads like a refusal. The demo scripts also refuse to start unless the
sequencer answers on `:3040` first, since starting the agent against a
not-yet-listening sequencer is the common way to trigger this.

**What would close it.** Upstream returning a transport error from
`privacy_preserving_tx` instead of unwrapping, so the module can surface a failed
transfer and stay alive. With that, the existing FSM path already handles it —
Pilot needs no redesign.

---

## 7. Inbound A2A was read, then thrown away — FIXED 2026-07-28

**What it was.** Two agents on the same Waku network never learned each other:
`agentDiscover("")` returned `{"count":0}` indefinitely. The same loss applied to an
inbound A2A **task**, so a paid task submitted over broadcast never completed — the
asker's row stayed `submitted` because no reply ever came.

**The previous entry here was wrong, and worth saying so.** It concluded the loss was
"on the receiving side, between B's Waku node holding the bytes and B's pilot module
being handed them", with the exact seam unpinned. Nothing was ever lost at that seam.
The module was handed **every** message and discarded them all itself.

**What it actually was — measured 2026-07-27.** `delivery_module` emits
`messageReceived` with FOUR arguments:

```
[0] message hash   "0xf44f9cc367c65b38…"
[1] content topic  "/pilot/1/discovery/proto" | "/pilot/1/inbox-…/proto"
[2] payload        BASE64 of what the sender passed to send()
[3] timestamp      "1785195659529573120"
```

The callback read `[0]` as the topic and `[1]` as the payload. So every comparison —
our inbox, the reply topic, the discovery topic, the owner channel — was made against
a message **hash** and could never match. Second defect in the same place: the payload
arrives **base64**-encoded, so fixing only the index would still have handed ECIES
decrypt and JSON parse the wrong bytes.

One mistake, and it accounts for every A2A symptom recorded on this branch: discovery
finding nobody, a task never reaching the doer, and a paid task never settling.

**How it was found, since the previous attempt failed.** The defect was invisible:
the module emits no log lines at all (zero `[pilot]` lines in a 550KB daemon log) and
`delivery_module` logs a `send` but never a `subscribe`, so "never delivered" and
"delivered then dropped" look identical from outside — both are silence. Recording
every inbound message to `delivery_events` **before any routing decision** separated
them in one run: the callback had fired 47 times while the agent acted on none of them.

**Verified after the fix.** Broadcast discovery passes for the first time —
`[B] discovers ≥1 agent` and `[B] discovered agent is A`, with the peer's payout and
declared price intact on the stored card. `delivery_events` records real content topics
instead of hashes. Phase 5 (task send / subscribe / cancel) passes end to end.

**Knock-on defects the fix exposed**, each fixed in turn: an agent that suddenly
processes real traffic does a database write per message on the thread that also
services owner and CLI calls; peers re-broadcast cards continuously, so every arrival
re-ran an ECDSA verification; and both the reply-topic subscribe and the test's own
call timeout were set to 120s, so the caller could give up while the agent was still
succeeding. Recording only `/pilot/1/` traffic, skipping byte-identical cards, and
letting the caller outlast the agent addressed those.

**Still open.** The paid task has not yet been observed completing end to end
(`A balance 100 -> 95` on a task). `agentTask` is instrumented with progress markers to
locate where it stops; the remaining failure is under active diagnosis, not unexplained.
Out-of-band import (`pilot peer add <card.json>`) remains available and does not depend
on discovery.

## How to read this list

- Items **1**, the upstream parts of **2.7**, and **6** are **platform gaps**,
  evidenced against pinned upstream revisions or against a captured crash — not
  Pilot defects.
- Items **2 (1–6)**, **3**, **4** and **5** are **Pilot's own scope and verification
  gaps**, stated so a follow-up question does not catch us out.
- Item **7** is **fixed** (2026-07-28) and kept here deliberately rather than deleted.
  Its earlier text asserted a cause that turned out to be wrong — that messages were
  lost between the peer's Waku node and its module. They never were: the module
  received all of them and discarded them itself, reading a message hash where the
  content topic was and never decoding the payload. The entry is left standing because
  a limitations list that quietly deletes its own mistakes is worth less than one that
  shows them.
- Where a fix path exists it is named. Where it depends on infrastructure that does
  not exist yet (a public testnet), that dependency is stated plainly.

If something you expected to find is missing from this list, it is an oversight, not
concealment — please raise it.
