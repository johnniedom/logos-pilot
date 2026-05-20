# Pilot — LP-0008 Sovereign Agent on LEZ

There is logos skills that cover most of how logos works, always looks through those skill when you're working on anything you aren't sure of, don't assume.

## Before Writing Code

Read `docs/research/` in this order: spec.md, decisions.md, audit-module-apis.md, research-hard-problems.md, research-identity-infrastructure.md. Implementation plan at `plans/pilot-implementation.md`.

## Build & Test

```bash
cd pilot-module && git init && git add -A && nix build
nix build .#unit-tests -L   # 44 tests — crypto, skills, LLM, core
```

### logoscore CLI (headless runtime)

The CLI is in a **separate repo**: `github:logos-co/logos-logoscore-cli` (NOT the old `logos-liblogos` library).
Ref: https://github.com/logos-co/logos-logoscore-cli

```bash
# Build logoscore CLI (one-time)
nix build 'github:logos-co/logos-logoscore-cli' --extra-experimental-features 'nix-command flakes'

# Install module as LGX package (required — logoscore uses lgpm package format)
LGPM=$(find /nix/store -maxdepth 3 -name "lgpm" -path "*/bin/*" 2>/dev/null | head -1)
nix build .#lgx --extra-experimental-features 'nix-command flakes' -o result-lgx
$LGPM install --file result-lgx/logos-pilot-module-lib.lgx \
  --modules-dir /tmp/pilot/modules --allow-unsigned

# Set LOGOS_HOST_PATH (required — logoscore spawns modules via logos_host)
export LOGOS_HOST_PATH=$(find /nix/store -maxdepth 1 -name "*-logos-liblogos" -type d | head -1)/bin/logos_host

# Inline mode — quick smoke test (confirmed working 2026-05-20)
logoscore -m /tmp/pilot/modules -l pilot -c "pilot.echo(hello)" --quit-on-finish
logoscore -m /tmp/pilot/modules -l pilot -c "pilot.metaSkills()" --quit-on-finish
logoscore -m /tmp/pilot/modules -l pilot -c "pilot.metaStatus()" --quit-on-finish

# Daemon mode — for persistent agent + pilot chat
logoscore -D -m /tmp/pilot/modules    # start daemon
logoscore load-module pilot           # load module
logoscore call pilot echo "hello"     # call methods
logoscore call pilot metaSkills       # no-arg methods
logoscore stop                        # shutdown
```

**IMPORTANT:** The `logoscore` binary in the nix store under `logos-liblogos-build` is the OLD core library runtime — it does NOT parse `-c`/`-l`/`-m` flags. Always use the CLI from `logos-logoscore-cli`. Bare `.so` files don't work — modules must be installed via `lgpm` (creates `manifest.json` + directory structure).

### Basecamp (GUI runtime)

Ref: https://github.com/logos-co/logos-basecamp (release 0.1.2-RC3)

```bash
nix build 'github:logos-co/logos-basecamp'
./result/bin/LogosBasecamp --user-dir /tmp/pilot-data
```

See `docs/DEVELOPER_GUIDE.md` for the full testing matrix.

## Rules

- **Universal C++ module.** Pure C++ only (std::string, int64_t, std::vector<T>). No Qt types in pilot_impl.h. Qt glue is auto-generated.
- **Dependencies (metadata.json):** `["lez_wallet_module", "delivery_module", "storage_module", "waku_module", "chat_module"]` — short-form names, verified from source.
- **Nix schema:** `"nix": { "packages": { "build": [], "runtime": [] }, "cmake": { "find_packages": ["SQLite3"] } }` — NOT "buildInputs".
- **Inter-module calls:** `logosAPI->callModule("module_name", "method", {args})` — Qt Remote Objects, not FFI (D11).
- **Identity:** `KeyChain::new_os_random()` via wallet-ffi. Do NOT use Python identity or custom key generation (prior submission rejected for this).
- **Owner channel:** Auto-establish via `requestMyBundle()` + `createConversation(bundle, "Pilot connected.")`. Text-only messages via `sendMessage(conversationId, content)`.
- **Spending FSM:** 9 states. Ambiguity defaults to inaction, NEVER execution. SQLite WAL persistence.
- **A2A transport:** JSON-RPC 2.0 over Waku. NATS reply pattern. `_logos` extension envelope.
- **Encryption:** ECIES for agent inboxes, chat_module E2E for owner, AES-256-GCM for files.
- **LLM:** Pluggable trait. AnthropicProvider + OpenAIProvider. No bundled model. `pilot deploy` uses arrow-key selector (inquirer/dialoguer style), not numbered input.
- **RISC0:** Only set `RISC0_DEV_MODE=0` when a SPEL guest binary exists.
- **Two GitHub orgs:** logos-co (modules) + logos-blockchain (LEZ wallet, key_protocol). Check both.

## Phase Order

0: Infrastructure → 1: Identity + Wallet → 2: Owner Channel → 3: Spending FSM → 4: Storage + Messaging (10 skills) → 5: A2A Transport (5 skills) → 6: Integration Tests → 7: Basecamp UI → 8: Ship

Do not reorder. Each phase depends on the previous. Full details in `plans/pilot-implementation.md` and `docs/research/decisions.md` (D10).
