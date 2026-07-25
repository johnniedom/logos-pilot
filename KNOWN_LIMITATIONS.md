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

**What would close them.** (1) an `owner-execute`/`owner-approve-task` command that
resumes a parked risky inbound task; (2) offloading `agent.ask` LLM execution off
the delivery thread; (3) an out-of-band identity attestation to defeat first-seen
squatting; (4) on-demand card fetch at settlement time; (5) finer-grained outbound
state journaling; (6) a documented canonical-JSON spec independent of Qt.

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

## 4. Build & test — CI-verified green; real on-chain settlement still manual

**What it is.** The module **builds**, the full **unit suite (109 tests) passes**,
and the **end-to-end job passes** in CI — pilot + its dependencies load into the
`logoscore` runtime against a **standalone LEZ sequencer**, the echo round-trip
works, and all 22 skills are present. (CI run on the default branch, all three jobs:
Build C++ Module, Build Nim CLI, E2E.) What is **not** yet exercised in automation is
**real on-chain settlement** — funding/spending with RISC0 proofs — and the **live
two-agent A2A round trip**; those remain the manual / video path.

**Why CI rather than local.** A local build is infeasible on the development box:
building `wallet-ffi` + RISC0 OOM-crashes the WSL VM, with no local Cachix / RISC0
substituter. CI (with the Cachix cache `logos-pilot-johnniedom` and a disk-freed
runner) is the source of truth — and is green.

**What would close the remainder.** A run that drives real funding/spending with
`RISC0_DEV_MODE=0` against a sequencer, plus a live two-agent A2A settlement, to
confirm the on-chain behavior the unit/E2E suites do not assert.

---

## 5. Testnet evidence & demo video — not met

The following submission criteria are **not satisfied**:

- **F9 — ≥ 3 use cases demoed end-to-end on testnet:** not met.
- **F10 — ≥ 5 third-party deployments:** not met.
- **Supportability #1 — public testnet deployment:** not met.

**Why.** Every demonstration to date runs against a **local sequencer**
(`run-sequencer.sh` in dev mode, `run-sequencer-realproof.sh` with real proofs),
not a public network.

An earlier revision of this document said no public LEZ testnet existed. **That is
no longer true**, and the correction matters more than the excuse: a public
endpoint is live at `https://testnet.lez.logos.co`, serving LEZ **v0.2.0**, and it
is **not** auth-gated (verified 2026-07-25 — a JSON-RPC POST returns a normal
`-32601 Method not found`; the auth wall is on the L1 testnet, which is a different
service). So the blocker on these three criteria is our own remaining work, not
missing infrastructure.

What is genuinely unknown, rather than assumed: whether the **pinned** wallet
module and circuits in this repo are wire-compatible with testnet v0.2.0, and how
an agent obtains an opening balance there — Pilot's self-funding uses a local
faucet claim that may have no public equivalent. Neither has been tested yet.

In addition:

- The `RISC0_DEV_MODE=0` real-proof scripts **are** committed
  (`run-sequencer-realproof.sh`, `demo-realproof.sh`, both with a `REHEARSE=1`
  dev-mode dry-run switch), but the end-to-end **demo video is not yet recorded /
  committed.**

**What would close it.** Point `wallet_config.json`'s `sequencer_addr` at the
public testnet, confirm version compatibility and a funding route, re-run the three
use cases there, and record the narrated real-proof video. Third-party deployment
numbers then become an outreach problem rather than an infrastructure one.

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

## How to read this list

- Items **1**, the upstream parts of **2.7**, and **6** are **platform gaps**,
  evidenced against pinned upstream revisions or against a captured crash — not
  Pilot defects.
- Items **2 (1–6)**, **3**, **4**, and **5** are **Pilot's own scope and
  verification gaps**, stated so a follow-up question does not catch us out.
- Where a fix path exists it is named. Where it depends on infrastructure that does
  not exist yet (a public testnet), that dependency is stated plainly.

If something you expected to find is missing from this list, it is an oversight, not
concealment — please raise it.
