# LP-0008 — Key Decisions Before Building

> Lock these before/during Phase 0. Stage 2 only — assumes LP-0017 already shipped or in-flight.

## D1. Repo strategy — separate repo, package-level coupling to Anchor

**REVISED 2026-05-05:** earlier draft proposed a shared monorepo with LP-0017. Johnnie corrected — these ship as **two independent products**. LP-0008 lives in its own repo, depends on Anchor's `anchor-index` crate via crates.io.

**Pick: new repo `johnniedom/pilot`** (or chosen LP-0008 name).

Reasons:
- Two distinct portfolio pieces — `johnniedom/anchor` and `johnniedom/pilot` each accumulate their own stars, issues, contributors
- Independent release cycles — Anchor can iterate on its own without touching Pilot
- If LP-0017 stalls or someone else submits first, Pilot is not entangled
- Each product has its own brand, its own README narrative, its own demo
- Standard package coupling beats monorepo coupling — `anchor-index` is a published Cargo crate, Pilot depends on it like any other library

Pilot's repo tree:

```
pilot/
├── Cargo.toml                  # depends on `anchor-index = "0.1"` from crates.io
├── apps/
│   └── basecamp-pilot/         # owner chat UI for the agent
├── crates/
│   ├── pilot-core/             # the agent runtime (Logos Core module + Qt Remote Objects)
│   ├── pilot-skills/           # the 12 default skills
│   ├── pilot-a2a/              # A2A transport binding over Waku
│   └── pilot-cli/              # `pilot deploy / start / stop`
├── programs/                   # any Pilot-specific LEZ programs (e.g. payment escrow if added later)
├── examples/
│   ├── skill-weather/          # third-party skill extension demo
│   └── use-case-{vault,alerter,multiagent}/
├── docs/
│   ├── architecture.md
│   ├── skill-interface.md
│   ├── payment-model.md
│   ├── distribution-plan.md
│   └── cu-benchmarks.md
├── scripts/
│   └── demo.sh
├── tests/
│   └── integration/
└── .github/workflows/
```

Storage skill imports the published crate:

```rust
// pilot/crates/pilot-skills/src/storage.rs
use anchor_index::{upload, download, IndexClient};

pub struct StorageSkill { client: IndexClient }
impl Skill for StorageSkill { /* ... */ }
```

**Action:** scaffold `johnniedom/pilot` (private) at the start of Phase 2, AFTER `anchor-index` v0.1.0 is published to crates.io.

## D2. Inference backend choice (NOT bundled)

Spec is explicit: "module must support pluggable inference (local or API-based), but the choice of model is left to the deployer."

**Pick: define a thin `LLMProvider` trait. Ship 2 default impls:**
- `AnthropicProvider` (Claude API — uses claude-agent-sdk)
- `OpenAIProvider` (any OpenAI-compatible endpoint — covers OpenRouter, local LM Studio, Ollama)

Don't ship a third (HuggingFace, etc) — keeps surface tight.

Configuration via env vars:
```
PILOT_LLM_PROVIDER=anthropic
ANTHROPIC_API_KEY=sk-...
PILOT_LLM_MODEL=claude-sonnet-4-6
```

**Deployment UX:** `pilot deploy` uses an arrow-key selector (inquirer/dialoguer style) for provider choice — not numbered input. Same pattern as domlabs-bot. User navigates with up/down arrows, confirms with Enter. API keys stored encrypted locally, never in config files or conversation history.

## D3. Skill interface design

Default ergonomics options:
- **Trait-based** (Rust idiomatic): every skill implements `Skill` trait with `execute(args) -> Result<Output>`
- **Function-pointer registry**: skills register by name, dispatch via lookup
- **MCP-style**: skills are sub-process MCP servers (heavy but interoperable)

**Pick: trait-based. Compile-time safety, easier IDE autocomplete, no IPC overhead.**

```rust
pub trait Skill {
    const NAME: &'static str;
    type Args: DeserializeOwned;
    type Output: Serialize;

    async fn execute(&self, ctx: &AgentContext, args: Self::Args) -> Result<Self::Output>;

    /// A2A-compatible JSON Schema for input validation
    fn input_schema() -> Value;
    /// A2A-compatible JSON Schema for output
    fn output_schema() -> Value;
    /// Optional LEZ price per invocation (for paid-skill marketplace use case)
    fn price_lez() -> Option<u64> { None }
}
```

Third parties add a skill by:
1. Implementing the trait in their own crate
2. Listing it in their agent's config TOML

## D4. Spending threshold storage + persistence

Above-threshold tx must wait for owner approval AND persist across restarts.

**Pick: SQLite-backed pending-tx queue.**
- Each pending tx has: id, tx_payload, requested_at, approval_state, expires_at
- On startup, agent reads queue, re-prompts owner for any pending entries
- Owner replies with `/approve <tx_id>` or `/reject <tx_id>` over the owner channel

## D5. Owner channel UX

Spec: "owner can interact with the agent in real time from a separate Logos app instance via Logos Messaging."

**Pick: Text-command chat UI in Basecamp.** Messages are text-only (confirmed from chat_module's `.rep` file: `sendMessage(conversationId, content)` takes strings only, no rich messages, no inline buttons). Approval prompts use text commands: `/approve`, `/reject`, `/pending`, `/limits`.

**Auto-establish on first boot:** Agent calls `requestMyBundle()` on chat_module via QtRO, receives bundle via `bundleReady()` signal, then calls `createConversation(bundle, "Pilot connected.")` on the owner's chat instance. Owner opens Basecamp Chat and sees the conversation already there. Zero manual steps.

**Fallback:** If cross-module conversation creation doesn't work (test in Phase 2), show the bundle string in the agent's Basecamp UI panel with a "Copy" button. Owner pastes it in Chat App's "New Conversation" screen (a dedicated input screen, NOT the message box — `createConversation()` takes the bundle as a function parameter).

## D6. A2A transport binding architecture

A2A defines HTTP transport. We replace HTTP with Waku.

**Pick: write an A2A `WakuTransport` adapter that satisfies the A2A SDK's transport interface.** Don't fork A2A — adapt it.

```rust
pub struct WakuTransport {
    waku_node: WakuClient,
    discovery_topic: String,
}

impl A2ATransport for WakuTransport {
    async fn send_request(&self, peer: AgentAddress, req: TaskRequest) -> Result<TaskResponse> {
        // serialize req → encrypted Waku message → wait for response on response topic
    }
    // ... other A2A transport methods
}
```

Reference: `shared/a2a-protocol.md` (Agent A2A research doc).

## D7. Payment timing — when does LEZ transfer happen?

A2A doesn't define payment. Options:
- **Pay-on-acceptance** — agent A pays B when B moves task into `working` state. Simpler. Risk: B accepts and never delivers, A loses funds.
- **Pay-on-completion** — agent A pays B when B moves into `completed` state. Better for A. Risk: B delivers but A doesn't pay.
- **Escrow via LEZ program** — a 3rd-party program holds A's payment, releases on B's completion proof or A's cancellation. Adds complexity.

**Pick: pay-on-acceptance for v1.** Simpler. The spec doesn't demand escrow. Document the trust assumption clearly. Note in `docs/payment-model.md`: "Future versions may introduce escrow-via-program for adversarial environments."

## D8. The 5-third-party-deployments problem

This is THE critical-path requirement. Cannot be hand-waved.

**Pick a multi-pronged distribution plan:**

- **Bounty (cheapest):** offer first 5 third-party deployers $20 USDC each (or sovereign tokens if launched). $100 total. Real money but tiny vs $1,200 prize.
- **Easy-mode quickstart:** the difference between "5 deployers" and "0 deployers" is whether deploying takes 5 minutes or 5 hours. Invest a full week in: one-command install, 5 pre-built example agents, video walkthrough.
- **Direct outreach:** DM 20-30 builders in Logos Discord, offer to pair-program their first deployment for free.
- **X / IndieHackers / HN:** post day-1 launch with demo gif. @domjohnnie + @domlabs_ amplify. Goal: 200 page views → 5 conversions.
- **Logos Discord builder-hub post:** "I shipped LP-0017 + LP-0008 — here's the agent SDK. Spin one up in 5 min, please." Existing Logos community is the warmest audience.

**Action:** start the distribution work IN PARALLEL with code. By the time the code is feature-complete, the deploy guide should already be tested by 1-2 friendlies.

## D9. Demo video plan

Spec: ≥3 narrated videos for ≥3 use cases. Suggested:

- **Video 1 (5-7 min): Personal file vault.** Owner sends file via Basecamp chat → agent encrypts, uploads to Codex, returns CID → owner retrieves from 2nd machine. (Reuses LP-0017 demo flow.)
- **Video 2 (5-7 min): On-chain event alerter.** Agent monitors a sample LEZ program → state change triggers Waku message → Basecamp chat shows alert.
- **Video 3 (8-10 min): Multi-agent paid task.** Agent A (translator client) discovers Agent B (translator) on a discovery topic → A pays B in LEZ → B translates → result back. Shows full A2A lifecycle + payment.

**RISC0 note:** Only show `RISC0_DEV_MODE=0` in videos that actually prove a SPEL guest binary (e.g., Video 3 if it submits a proved on-chain transaction). Do NOT set the flag in demos that only exercise messaging, storage, or A2A coordination. Setting it without a guest binary signals you don't understand what RISC0 does.

## D10. Order of operations within Phase 2

Critical-path ordering:

1. Logos Core module skeleton + agent identity + owner channel — get a heartbeat working
2. Storage skill (reuse LP-0017 module) — proves end-to-end flow with one skill
3. Messaging + meta skills — round out basic operation
4. Wallet + program skills + spending threshold — money flowing
5. A2A binding + agent.* skills — interop layer
6. Basecamp agent UI — owner chat experience
7. Use case 1 demo + recording (file vault — easiest, validates flow)
8. Use case 2 demo + recording (event alerter)
9. Use case 3 demo + recording (multi-agent paid task — hardest)
10. Distribution push + 5 third-party deployments

Don't reorder — each phase unblocks the next.
