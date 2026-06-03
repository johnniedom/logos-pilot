# Pilot — Demo Shot List (what to demo)

Action/step list for the submission video. **No narration here** — just what to run and what
appears on screen. Record with `RISC0_DEV_MODE=0` so the sequencer terminal shows real proofs.

**Prerequisite (one command brings up the whole stack + funds the agent):**
```bash
bash ~/demo-run.sh 0        # 0 = real proofs (use 1 to rehearse fast)
```
Keep the **sequencer terminal visible** on screen — that's where real proof generation shows.

Status legend: ✅ verified working · 🟡 logic works, transfer broken (fix first) · 🔴 not yet verified

---

## Demo 1 — Deploy & sovereign funding ✅
| # | Action (run) | On screen |
|---|---|---|
| 1 | `logoscore call pilot initialize /tmp/pilot-agent` | one command deploys the agent |
| 2 | `logoscore call pilot getAccountId` | agent's own account id |
| 3 | `logoscore call pilot walletBalance` | balance `100` (shielded) |
| 4 | (point to sequencer terminal) | real proof generated for the funding transfer |

## Demo 2 — Personal file vault ✅
| # | Action (run) | On screen |
|---|---|---|
| 1 | `echo "secret doc" > /tmp/vaultfile.txt` | a plaintext file exists |
| 2 | `logoscore call pilot storageUpload /tmp/vaultfile.txt "tax-2026"` | returns `cid`, `encrypted: true` |
| 3 | `logoscore call pilot storageList` | the file listed by label + cid |
| 4 | `logoscore call pilot storageDownload <cid> /tmp/out.txt` | `decrypted: true` |
| 5 | `diff /tmp/vaultfile.txt /tmp/out.txt` | identical — round-trip works |

## Demo 3 — Spending threshold ✅ (verified: 30 auto-spends, 60 held→approved→executed)
| # | Action (run) | On screen |
|---|---|---|
| 1 | `logoscore call pilot setSpendingLimits 50 200 86400` | per-tx 50, per-period 200 |
| 2 | `logoscore call pilot walletSend <recipient> 30 "small"` | executes autonomously (below limit) |
| 3 | `logoscore call pilot walletSend <recipient> 100 "big"` | `status: held` + `request_id` (above limit) |
| 4 | `logoscore call pilot getPendingSpends` | the 100 shown as `NOTIFIED`, awaiting approval |
| 5 | `logoscore call pilot approveSpend <request_id>` | owner approves → executes |

## Demo 4 — Paid agent marketplace / 2-agent A2A ✅ (verified: discover→message→task→pay, 13/14; run via `test-two-agents-docker.sh`, needs nwaku container up)
| # | Action (run) | On screen |
|---|---|---|
| 1 | Agent B: `logoscore call pilot agentCard` | B advertises a skill + LEZ price |
| 2 | Agent A: `logoscore call pilot agentDiscover "pilot"` | A finds B's signed Agent Card |
| 3 | Agent A: `logoscore call pilot agentTask <B_addr> <skill> '<params>'` | A pays the price, sends the task |
| 4 | Agent A: `logoscore call pilot agentSubscribe <B_addr> <task_id>` | lifecycle working → completed |
| 5 | Agent B: `logoscore call pilot walletBalance` | B's balance rose — payment settled |

---

### Recordable now
Demo 1 + Demo 2 (both ✅).

### Blocked until fixed/verified
- Demo 3: fix `transfer_private` (recipient private account likely needs on-chain registration).
- Demo 4: verify the 2-agent discover → task → pay flow on the v0.1.2 build.

(Interface coverage: film Demo 2 via **Basecamp** owner channel, Demos 1/3/4 via **CLI** — satisfies both required interfaces.)
