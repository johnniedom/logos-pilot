# LP-0008 Sovereign Agent — Submission-Readiness Brief
_Date: 2026-06-07 · Repo (local, WSL): `~/dev/logos/logos-pilot` · Branch: `feat/sovereign-funding`_

**Verdict: ~42% ready. NOT submittable today. Race is CLEAR (no live competitor PR).**

---

## 1. App-flow refresher (re-onboard in 5 minutes)

**One sentence:** Pilot is a C++ Logos Core module that turns the Logos stack into an autonomous AI agent — its own shielded LEZ wallet + encrypted Storage + Messaging + A2A coordination — driven by an owner over an ECIES-encrypted Waku channel, gated by a 9-state spending threshold, and operated via a Nim CLI (`pilot deploy` / `pilot chat`) or a Basecamp QML UI.

**The two paths (don't confuse them — this is the #1 trap):**
- **WORKING demo path** (use this for anything live): local-compiled LEZ v0.1.2 `sequencer_service` on **:3040** + pre-built LGX files, driven by the home scripts `~/demo-run.sh` and `~/run-a2a.sh`.
- **Documented path** (README/deployment-guide): Docker sequencer on **:8080** + nix-cache modules. Looks clean but is not the proven path. Trust the home scripts for demos.

**Fastest way back in:** open `wsl -d Ubuntu`, then `bash ~/demo-run.sh` (clean → install modules → start sequencer :3040 → start daemon → init pilot → fund agent to balance 100). Append ` 0` for real STARK proofs (`RISC0_DEV_MODE=0`, ~40 min on the 7.6GB box).

**Owner interaction:** two interfaces over the SAME `pilot.db` — `pilot chat` (Nim REPL, LLM + slash commands) and the Basecamp QML UI. **Never run both at once** (they collide on the daemon/db). Slash commands bypass the LLM and hit the module directly: `/balance /history /send <to> <amt> <reason> /approve <id> /reject <id> /upload /download /files /skills /status /discover /pending /help /quit`.

**The spending gate (the security boundary):** below per-tx limit → executes autonomously; above limit → module returns `{status:'held', request_id}` → FSM `CREATED→HELD→NOTIFIED`, agent ECIES-notifies owner over Waku → owner `/approve <id>` → `APPROVED→EXECUTING→COMPLETED` (real zk transfer) or `/reject` → `REJECTED`; timeout → `EXPIRED`. State persisted in SQLite, recovered on restart.

**Architecture in one breath:** Pilot is a **replica** of every dependency module. Wallet ops → `lez_wallet_module`. Owner channel + A2A + messaging skills → all ride **`delivery_module`** (one Waku transport) with Pilot's own ECIES (secp256k1 ECDH → SHA256 → AES-256-GCM). Storage → `storage_module` chunked upload/download. **`chat_module` is listed as a dependency but BYPASSED** (its callMethod is empty/broken). Persistence is SQLite WAL `pilot.db` (tables: `agent_identity`, `owner_channel`, `spend_requests`, `stored_files`, `config`). Source layout: `pilot-module/src/pilot_*.cpp` (identity/owner/spending/storage/messaging/a2a/meta/crypto/skill/llm).

**Skills (21 total, all registered):** 4 storage (upload/download/list/share), 3 messaging (send/join/create_group), 6 blockchain (wallet.balance/send/history + program.query/call/deploy), 5 A2A (card/discover/task/subscribe/cancel), 3 meta (skills/status/configure).

**Top gotchas:** set `PILOT_NAT=extip:127.0.0.1` in WSL (skips 12s NAT probe); first Waku join takes 30–60s (use slash commands while it warms); two storage modules can't share one host's LevelDB cache → Agent B runs in Docker for A2A; never delete `pilot.db` or `wallet_storage/` (holds NPK keypair + file AES keys); `/tmp` is wiped on WSL reboot but the `~/*.sh` launchers survive.

---

## 2. Gap analysis vs spec — done / partial / missing

### DONE (real, evidenced in code)
- **Logos Core module packaging** — `pilot-module/metadata.json`, `pilot-ui/src/pilot_ui.rep`, `flake.nix` `.#lgx`. Consumes official modules, patches none.
- **Shielded LEZ account + sovereign self-funding** — `pilot_identity.cpp:233` `createIdentity()`, `:303` `fundAgentIfNeeded()` (register → pinata faucet → shielded transfer, idempotent). Commit `6903936`.
- **Spending threshold FSM** — `pilot_spending.cpp` per-tx + per-period limits, HELD→NOTIFIED→EXECUTING→COMPLETED/TX_FAILED, `createSpendRequest/approveSpend/rejectSpend/getPendingSpends`, `recoverPendingTransactions()` for restart recovery.
- **Skill SDK** — `pilot_skill.h` (`PilotSkill` + `LambdaSkill`), `pilot_skill_registry.cpp`, `docs/skill-interface.md`, reference `examples/skill-weather/`.
- **Storage skills (4)** + **Messaging skills (3)** — `pilot_storage.cpp`, `pilot_messaging.cpp`, real `storage_module` / `delivery_module` RPC.
- **wallet.balance/send/history** — `pilot_identity.cpp` + `pilot_spending.cpp`.
- **agent.discover** — `pilot_a2a.cpp:147`.
- **Meta skills (3)** — `pilot_meta.cpp:11/19/44`.
- **Docs** — README 17KB + `docs/`: architecture, skill-interface, owner-channel, owner-guide, deployment-guide, payment-model, security-model, DEVELOPER_GUIDE (33KB).

### PARTIAL (built but not proven / not to spec)
- **Module coexistence on clean node** — code consumes official modules but no evaluator-facing log proves no-patch coexistence.
- **Owner channel** — works (`pilot_owner.cpp:13`), but topic derived from `agentAccountId_` only, not `(agent_id, owner_id)` as checklist asks; no 2nd-device reachability output.
- **program.query/call/deploy** — `pilot_a2a.cpp:406/432/473` implement all three, BUT carry the code's own error string "wallet-ffi does not yet support program queries/calls" — **not proven working on testnet** (LEZ v0.2.0-rc3 feature).
- **agent.card** — `pilot_a2a.cpp:40` builds card, but no confirmed cryptographic signature, no per-skill LEZ price wired to actual payment, A2A schema conformance unconfirmed.
- **agent.subscribe / agent.cancel** — `pilot_a2a.cpp:307/358` client-side sends only; no server emits streaming updates or honors cancel/refund.
- **Multi-agent A2A demo** — `test-two-agents-docker.sh` proves message plumbing + a SEPARATE manual `walletSend`, NOT payment-bound-to-task-acceptance.
- **3 use-case demos** — only funding/transfer segment runs `RISC0_DEV_MODE=0`; vault/messaging/A2A run dev-mode=1. UC2 (on-chain event alerter) and UC3 (multi-agent workflow) not implemented.
- **Reliability** — restart recovery of spend FSM exists; **no** exponential-backoff retry when owner unreachable (`sendToOwner` is one-shot best-effort, swallows exceptions), **no** A2A-task persistence, **no** skill-isolation test.

### MISSING (0% — these are the gating items)
- **agent.task economic loop** — `pilot_a2a.cpp:230` `agentTask()` is CLIENT-only, returns `state='submitted'`. There is **NO inbound task server**: `messageReceived` in `pilot_impl.cpp:157` is gated `if (topic != ownerChannelId_) return;` so incoming peer tasks are **dropped**. No `working`/`input-required` states anywhere in `pilot-module/src`. No autonomous LEZ pay on acceptance. **This breaks the headline criterion.**
- **5 third-party deployments** — zero evidence anywhere; `docs/distribution-plan.md` doesn't exist. Hardest deliverable, at 0%.
- **CI green on default branch** — `git remote -v` empty, default branch never pushed, CI has never run. `.github/workflows/ci.yml` runs only unit tests (no sequencer E2E).
- **CU benchmarks** — `docs/cu-benchmarks.md` does not exist.
- **Narrated demo videos** — none recorded/linked.
- **LICENSE files** — README claims "MIT OR Apache-2.0" but no `LICENSE-MIT` / `LICENSE-APACHE` exist; repo not public; PR not opened.

### Prioritized punch-list (do in this order)
1. **[CODE BLOCKER] Build the inbound A2A task server.** Un-gate `messageReceived` (`pilot_impl.cpp:157`) so it also handles the agent's own inbox topic; on a `tasks/send`, dispatch the skill via the registry, emit `working → completed/failed`, deliver result to the peer's ECIES reply topic, and make the LEZ payment atomic with task acceptance (route through the threshold FSM if above limit). Everything below depends on this.
2. **[CODE, 1 day] Reliability hardening** — add exponential-backoff retry + timeout report to `sendToOwner`; persist + recover pending A2A tasks; add one skill-failure-isolation test.
3. **[CODE, small] Approval timeout job** — implement the auto-cancel timer the owner message already advertises ("Expires: 60 min").
4. **[CONTENT] CU benchmarks** — create `docs/cu-benchmarks.md`: measured CU for token transfer / program call / deploy, each with commit SHA + timestamp + `RISC0_DEV_MODE=0` confirmation.
5. **[INFRA] Publish repo + green CI** — add LICENSE files, `.gitignore` audit, push to GitHub, confirm Actions green on `main`. Add a sequencer-standalone E2E job covering the 21 skills + threshold + A2A.
6. **[DISTRIBUTION — longest lead] 5 third-party deployments** — quickstart + prebuilt configs + outreach; collect 5 attestations / on-chain records. Start outreach NOW in parallel with code; this is the long pole.
7. **[CONTENT] Record 3 narrated `RISC0_DEV_MODE=0` videos** — only after the demo script is green end-to-end.
8. **[HONESTY] Update A2A + program docs** to reflect inbound-task/payment status and the wallet-ffi program limitation (winners are rewarded for documented limitations, not punished).

---

## 3. How to make the PR

> **Do NOT open the final PR until items 1, 5, 6, 7 above are real.** FCFS rewards the first COMPLETE submission; an incomplete PR burns 1 of 3 submissions + your 1-per-week review slot. A **DRAFT** PR is acceptable purely to timestamp the claim.

**Submission shape (from accepted winners):** the lambda-prize PR adds exactly ONE file — `solutions/LP-0008.md` — built verbatim from `solutions/LP-0000.md`. The code lives in your external public repo, only linked. PR title: **`Solution: LP-0008 — Sovereign Agent`**.

### A) Publish the pilot repo (currently has no remote)
```bash
# Licenses first (dual MIT + Apache-2.0 is mandatory). In WSL at repo root:
wsl -d Ubuntu -- bash -lc 'cd ~/dev/logos/logos-pilot && curl -sL https://www.apache.org/licenses/LICENSE-2.0.txt -o LICENSE-APACHE'
# Create LICENSE-MIT with standard MIT text, "Copyright (c) 2026 Johnnie Modebe".
# Audit nothing secret/large is tracked:
wsl -d Ubuntu -- bash -lc 'cd ~/dev/logos/logos-pilot && git status --porcelain && git ls-files | grep -iE "key|secret|result/|store/" || true'
wsl -d Ubuntu -- bash -lc 'cd ~/dev/logos/logos-pilot && git add LICENSE-MIT LICENSE-APACHE .gitignore && git commit -m "chore: add MIT + Apache-2.0 dual license"'
# Create the public repo (run on Windows where gh is authed as johnniedom):
gh repo create johnniedom/pilot --public --description "Sovereign Agent — autonomous AI Logos Core module (LP-0008)" --disable-wiki
# gh is NOT in WSL — inject token from Windows: TOKEN=$(gh auth token)
wsl -d Ubuntu -- bash -lc 'cd ~/dev/logos/logos-pilot && git remote add origin https://johnniedom:'"$TOKEN"'@github.com/johnniedom/pilot.git && git push -u origin feat/sovereign-funding:main'
gh repo edit johnniedom/pilot --default-branch main
# Then scrub the token from the remote URL.
```
**Repo-name decision:** existing draft + docs hardcode `johnniedom/pilot`. **Keep `pilot`** to avoid rewriting every URL.

**Confirm:** CI workflow triggers reference `main` so "CI green on default branch" is satisfied after push. Then clone fresh into `/tmp` and run `./demo.sh` with `RISC0_DEV_MODE=0` UNMODIFIED — evaluators do exactly this.

### B) Fork lambda-prize and open the PR (Windows / Bash tool)
```bash
gh repo fork logos-co/lambda-prize --clone --remote
cd lambda-prize
git checkout -b solution/lp-0008-sovereign-agent master
# Create solutions/LP-0008.md from LP-0000 template (filled — see below)
git add solutions/LP-0008.md
git commit -m "Solution: LP-0008 — Sovereign Agent"
git push -u origin solution/lp-0008-sovereign-agent
gh pr create --repo logos-co/lambda-prize --base master --head johnniedom:solution/lp-0008-sovereign-agent --title "Solution: LP-0008 — Sovereign Agent" --body-file pr-body.md
```
Then post the narrated `RISC0_DEV_MODE=0` video as a PR comment. Expect **multiple review rounds** — post running "✅ update" comments as each criterion lands; rebase onto upstream HEAD if required before merge. **Claim payment only AFTER merge** via the Lambda Prize payment issue template (full legal name "Johnnie Modebe", country, single-use ETH address — it becomes public).

### Ready-to-fill `solutions/LP-0008.md` template
Follow `solutions/LP-0000.md` verbatim: `# Solution: LP-0008 — Sovereign Agent` → **Submitted by:** Johnnie Modebe (johnniedom) → **Summary** → **Repository** (`- **Repo:** https://github.com/johnniedom/pilot` — public, dual MIT + Apache-2.0) → **Approach** (incl. "why the Logos stack" + "alternatives considered / what didn't work") → **Key files** bullet list (path → what it proves) → **Related issues / Logos stack fit** → **Success Criteria Checklist** (mirror ALL ~22 LP-0008 boxes, each `[x]` with a one-line evidence pointer to a file/test/video timestamp) → **FURPS Self-Assessment** (5 prose subsections with concrete numbers: 44 unit tests, 28 single-agent integration, 14 two-agent, CU figures, latencies) → **Supporting Materials** (3 video URLs, docs links) → **Note on limitations** (program.query/call wallet-ffi gap; dev-mode vs production-ZK failure-path note; what the agent can/cannot do without owner approval) → **Terms & Conditions** line linking TERMS.md.

Fill before filing: 3 video URLs, Agent A/B testnet refs + owner-channel topic id, CU table (3 rows), the 5 third-party deployment rows (deployer handle → skill → on-chain tx / attestation URL).

---

## 4. Demo blueprint (derived from accepted submissions)

Record ONE narrated master video (or 3 chaptered clips). Winners' non-negotiables: **narrate architecture + decisions before any terminal action**, and **prove `RISC0_DEV_MODE=0` on camera** (LP-0012's winning comment explicitly showed local sequencer → tx included → receipt → decoder).

1. **Open (60–90s architecture):** show the Logos Core module loading alongside wallet/storage/messaging, the skill interface, the threshold design, the A2A Agent Card / task lifecycle. State what + why first.
2. **Prove dev-mode=0 early:** `echo $RISC0_DEV_MODE` → 0, start `~/run-sequencer-realproof.sh` (:3040), let r0vm proving output scroll — that output IS the evidence.
3. **Single-command deploy:** owner runs ONE CLI command on a headless node → agent prints its shielded LEZ account + messaging address. Switch to a SEPARATE Logos app instance, chat over the owner channel, no intermediary server.
4. **UC1 — Personal file vault (Storage):** owner sends a file → `storage.upload` returns content address → `storage.download` from a second path → byte-identical retrieval.
5. **UC2 — On-chain event alerter + threshold (Messaging + Blockchain):** agent watches a LEZ program via `program.query`; on change it messages the owner. THEN trigger a `wallet.send` ABOVE the limit → agent PAUSES, sends approval request to owner chat → owner approves → tx submits; also show a below-threshold send executing autonomously. _(Requires program.query actually working; if unresolved, swap UC2 for a pure threshold demo and document the limitation.)_
6. **UC3 — Multi-agent A2A marketplace (A2A + payment):** second specialist agent publishes an Agent Card with a LEZ price to a discovery topic → client runs `agent.discover` → `agent.task` through `working → completed` → autonomous `wallet.send` paying the price (no human). Show both balances before/after + the task states on screen. _(Gated on punch-list item #1 — cannot be filmed until the inbound task server + pay-on-acceptance exist.)_
7. **Show three distinct agent deployments** (one per category: Storage, Messaging, Blockchain), each with its own deploy command + distinct visible identity/address.
8. **Close:** show CI green (Actions tab) + run the reproducible `./demo.sh` from a clean checkout unmodified. Host on YouTube, link in Supporting Materials + PR comment.

On-screen at all times: terminal/log pane (proves real execution + dev-mode=0), owner Logos chat pane, relevant skill/RPC output. Narrate each call by name (`storage.upload`, `agent.task`, `wallet.send`) so it maps 1:1 to the prize's skill list.

---

## 5. Competition check + race verdict + recommended next moves

**Verdict: CLEAR — the lane is OPEN, not "we're ahead."** As of 2026-06-07 there is no live LP-0008 PR, no LP-0008 issue, and no accepted `solutions/LP-0008.md`.

- **Beach-Bum (Ned Karlovich) — "Agora" / agora-agent:** the ONLY LP-0008 PR ever (#34, claimed 21 skills / A2A v1.0.0 / shielded wallet / owner channel / spending policy) was **rejected/closed 2026-04-28**. No resubmission since; his lambda-prize fork is dormant. His `agora-agent` repo saw post-rejection iteration (last pushed 2026-05-13). Latent threat only. **Action: mine his closed PR's review comments as a free spec-compliance checklist** — likely failed on RISC0_DEV_MODE=0 testnet proof gaps, the 5-deployment bar, or CI/video evidence (exactly your current gaps).
- **Qt/C++ plugin-scaffold competitor (Discord intel, 2026-05-25):** invisible on GitHub — no PR, no fork branch, no public repo. Could surface a PR with little warning. Biggest blind spot. **Action: run a discrawl check on Logos Discord to pin down handle + progress.**
- **retraca (Gon):** ruled OUT — working on LP-0013 / LP-0016, no agent work.

**Race assessment:** substantial runway, but you hold NO submission yet either, so "clear" = empty field, not a lead. The practical race is to be first to open a COMPLETE PR.

**Recommended next moves (this week):**
1. **Claim the lane cheaply now:** drop intent in Logos Discord #builder-hub (confirm LP-0008 isn't in-flight) and optionally open a DRAFT PR to timestamp. Lowercase/conversational builder voice, no Dom Labs mention.
2. **Start the 5-deployment outreach immediately** — longest lead item, zero code dependency.
3. **Build the inbound A2A task server (punch-list #1)** — unblocks UC3, the headline criterion, and the whole A2A economic loop.
4. **Read Beach-Bum #34's close comments** before writing your checklist — replicate strengths, avoid his failure mode.
5. **Set a daily watch:** re-poll `gh pr list --repo logos-co/lambda-prize --state all` + the fork list. A new `lp-0008` branch or a push to Beach-Bum's lambda-prize fork is your earliest warning.
6. **Don't over-polish** — once all criteria are genuinely met, file fast. Completeness + early timestamp beats extra polish (LP-0010 won only because the earlier competitor was rejected).
