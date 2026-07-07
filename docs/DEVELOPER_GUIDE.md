# Logos Module Developer Guide

Hard-won lessons from building LP-0008 (Pilot Agent Module). Everything here was figured out by reading source code, SDK headers, and existing modules — it's not documented elsewhere.

## The Real API (not what you'd expect)

The Logos docs and specs reference `logosAPI->callModule("module", "method", {args})`. **This does not exist.** The actual pattern is:

```cpp
// WRONG — this API doesn't exist
LogosResult result = logosAPI_->callModule("delivery_module", "send", {topic, payload});

// RIGHT — how it actually works
auto* client = logosAPI_->getClient("delivery_module");
QVariant result = client->invokeRemoteMethod("delivery_module", "send", topic, payload);
```

### Key differences from what you'd expect

| What you'd guess | What actually works |
|---|---|
| `logosAPI->callModule(...)` | `logosAPI->getClient("module")->invokeRemoteMethod(...)` |
| `LogosResult` with `.success()` | `QVariant` — check `.isNull()` for failure |
| `result.data().toString()` | `result.toString()` directly |
| `result.errorMessage()` | Doesn't exist — no error message accessor |
| `setLogosAPI(LogosAPI* api)` | `onInit(QVariant api)` — code generator wraps it |

### Where to find the real API

After your first `nix build` (even if it fails), the SDK headers are cached in the nix store:

```bash
# Find the SDK headers
find /nix/store -name "logos_api.h" -path "*/cpp/*" 2>/dev/null

# Key files to read:
# logos_api.h        — LogosAPI class (getClient, getProvider)
# logos_api_client.h — LogosAPIClient (invokeRemoteMethod, onEvent)
# logos_result.h     — StdLogosResult (for return types, NOT for inter-module calls)
```

### The onInit problem

The code generator wraps every method parameter as `QVariant`. For `onInit`, it generates:

```cpp
void PilotProviderObject::onInit(QVariant api) {
    m_impl.onInit(api);  // passes QVariant, not LogosAPI*
}
```

So your impl must accept `QVariant`:

```cpp
// In header
void onInit(QVariant api);

// In implementation
void MyImpl::onInit(QVariant api) {
    logosAPI_ = static_cast<LogosAPI*>(api.value<void*>());
}
```

### Reference implementation

The anchor module (LP-0017) is the best working reference:

```
~/dev/logos/logos-anchor/anchor-module/src/anchor_impl.h   — header
~/dev/logos/logos-anchor/anchor-module/src/anchor_impl.cpp — implementation
```

It shows the correct `onInit`, `getClient`, and `invokeRemoteMethod` patterns.

## Building

### Build targets

All from `pilot-module/` directory:

```bash
nix --extra-experimental-features "nix-command flakes" build          # .so binary
nix --extra-experimental-features "nix-command flakes" build .#lgx    # .lgx installable package
nix --extra-experimental-features "nix-command flakes" build .#install       # module + manifest
nix --extra-experimental-features "nix-command flakes" build .#unit-tests -L # run tests
```

### The `result/` symlink

`result/` only holds the LAST thing you built. `nix build` → `.so`. `nix build .#lgx` → `.lgx`. They overwrite each other.

### First build is slow, subsequent builds are fast

The first `nix build` downloads and compiles the entire dependency tree — Rust toolchain, Qt, all Logos modules. This takes 1-3 hours depending on your machine. After that, everything is cached in `/nix/store/` and rebuilds only recompile YOUR code (~2 minutes).

### The dirty git tree warning

Nix warns if you have uncommitted changes. It still builds, but uses the dirty tree. Commit before building for reproducible builds:

```bash
git add -A && git commit -m "wip"
nix build
```

## Inter-Module Communication (Qt Remote Objects)

### Connection readiness

Modules run in separate `logos_host_qt` processes. They communicate via Qt Remote Objects replicas. After `load-module`, the replica connection takes time to establish. Calling `invokeRemoteMethod` before the replica is ready causes a **segfault** (SIGSEGV in `libQt6RemoteObjects.so` at offset 0x60).

**Always check `isConnected()` before calling `invokeRemoteMethod`:**

```cpp
#include "logos_api_client.h"
#include <thread>
#include <chrono>

auto* wallet = logosAPI_->getClient("lez_wallet_module");
if (!wallet) return false;

// Wait for replica connection (max 5s)
for (int i = 0; i < 20 && !wallet->isConnected(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

if (!wallet->isConnected()) {
    qWarning() << "wallet module not connected";
    return false;
}

// Now safe to call
QVariant result = wallet->invokeRemoteMethod("lez_wallet_module", "open", configPath, storagePath);
```

### Available `LogosAPIClient` methods

From the SDK headers (`logos_api_client.h`):

| Method | Returns | Description |
|--------|---------|-------------|
| `isConnected()` | `bool` | Whether the Qt Remote Objects replica is connected |
| `reconnect()` | `bool` | Attempt to re-establish the connection |
| `invokeRemoteMethod(obj, method, args...)` | `QVariant` | Synchronous RPC call (up to 5 arg overloads) |
| `invokeRemoteMethodAsync(obj, method, args..., callback)` | `void` | Async RPC with callback |
| `requestObject(objectName)` | `LogosObject*` | Get a handle for event subscriptions |
| `onEvent(object, eventName, callback)` | `void` | Register an event listener |
| `getToken(moduleName)` | `QString` | Get capability token for a module |

### The `onInit` hook

The code generator produces `onInit(QVariant)` which hides the base class's `onInit(LogosAPI*)`. To receive the `LogosAPI*` pointer, inject an override via `sed` in `flake.nix` `preConfigure`:

```bash
sed -i '/^private:/i\    void onInit(LogosAPI* api) override {\n        m_impl.logosAPI_ = api;\n    }\n' \
  ./generated_code/pilot_qt_glue.h
```

This runs at build time. The framework calls `onInit` after module load, passing the API pointer.

## Daemon Process Management

### Module host processes

Each loaded module runs in its own `logos_host_qt` process. Killing `logoscore` does NOT kill the module hosts — they become orphans holding locks and sockets.

**Proper cleanup:**

```bash
# Kill everything
pkill -9 -f logos_host_qt
pkill -9 -f logoscore

# Remove stale locks
rm -f ~/.cache/storage/dht/providers/LOCK
rm -rf /tmp/pilot-data/.logoscore/daemon
```

### CLI and Basecamp cannot run simultaneously

Both the CLI daemon (`logoscore -D`) and Basecamp load the same modules into separate `logos_host` processes. They compete for the same Qt Remote Objects sockets and SQLite database locks. Running both at once causes one or both to crash.

**Rule: one runtime at a time.**

- Close Basecamp before running `pilot deploy` or `pilot chat`
- Exit the CLI (`/quit` or Ctrl+C) before opening Basecamp

Both runtimes share the same `pilot.db` at `PILOT_DATA_DIR` (default `/tmp/pilot-data`). Data set in the CLI (identity, LLM key, files, name) is available in Basecamp on next launch, and vice versa.

**If deploy fails with "Identity generation failed"**, check for stale Basecamp processes:

```bash
# Check for lingering logos_host processes
ps aux | grep logos_host

# Kill everything
pkill -9 -f logos_host
pkill -9 -f logoscore
sleep 2

# Now deploy works
./pilot-cli/result/bin/pilot deploy
```

### Module crash on cold start

Modules sometimes crash on the first startup after a clean data wipe. This is a transient race condition in the Logos runtime — different modules crash each attempt (capability_module, chat_module, lez_wallet_module).

**Fix:** Run `./setup-modules.sh` to do a clean reinstall with smoke test, then deploy:

```bash
./setup-modules.sh        # reinstalls all modules + runs echo test
./pilot-cli/result/bin/pilot deploy
```

If deploy still fails, retry — the second or third attempt typically succeeds.

### Wallet storage persistence

The `lez_wallet_module` stores wallet state in `$PILOT_DATA_DIR/wallet_storage/`. If this directory is deleted but `pilot.db` still has the old account ID, the wallet module reports "Null wallet handle" and balance calls fail.

**Fix:** Delete both together — never delete `wallet_storage/` without also deleting `pilot.db`:

```bash
rm -rf /tmp/pilot-data    # clean wipe, then redeploy
./pilot-cli/result/bin/pilot deploy
```

### Basecamp module variant mismatch

Nix builds produce modules with variant `linux-amd64-dev`. Basecamp expects `linux-amd64`. The `install-basecamp.sh` script auto-patches this, but if you install modules manually, fix the variant:

```bash
echo -n "linux-amd64" > ~/.local/share/Logos/LogosBasecamp/modules/pilot/variant
sed -i 's/linux-amd64-dev/linux-amd64/g' ~/.local/share/Logos/LogosBasecamp/modules/pilot/manifest.json
```

### Basecamp QML cache

After updating plugin QML files, clear the cache or changes won't appear:

```bash
rm -rf ~/.cache/Logos/LogosBasecamp/qmlcache
```

### Basecamp plugin discovery

Basecamp requires three files for a QML plugin to appear in the sidebar:
- `manifest.json` — full manifest with hashes
- `metadata.json` — simplified metadata for the package manager
- `variant` — platform string (`linux-amd64`)

Missing any of these causes the plugin to silently not appear.

### LevelDB lock contention

The `storage_module` uses LevelDB at `~/.cache/storage/dht/providers/`. If a previous `logos_host_qt` process holds the lock, the new storage_module crashes:

```
ERR Failed to initialize discovery datastore
    path=~/.cache/storage/dht/providers
    err="IO error: lock .../LOCK: Resource temporarily unavailable"
[critical] Module process crashed: storage_module
```

**Fix:** Kill stale `logos_host_qt` processes and remove the lock file before starting a new daemon.

### Daemon PID tracking

The daemon writes its PID to `<config-dir>/daemon/state.json`. When using `setsid` to detach the daemon (required when launching from a non-interactive shell), `$!` captures the `setsid` wrapper PID, not the daemon PID. Read from `state.json` instead:

```json
{
    "pid": 36685,
    "instance_id": "e986e5400bf4",
    "started_at": "2026-05-25T16:11:41Z",
    "version": 2
}
```

### Timeouts for inter-module initialization

When calling `initialize()` on a module that depends on other modules (e.g., pilot → wallet → sequencer), allow sufficient time:

| Operation | Typical time | Recommended timeout |
|-----------|-------------|-------------------|
| Qt RO replica connection | 1-5s | 5s poll loop |
| Wallet `create_new` (talks to sequencer) | 3-8s | 15s |
| Full `initialize()` (connection + wallet + keys) | 5-15s | 30s |
| Delivery module startup (NAT detection + peer discovery) | 8-12s | 15s |
| LLM API call (Gemini/OpenAI/etc.) | 2-15s | 30s |

### Nix store binary discovery

Binaries are at depth 3: `/nix/store/<hash>/bin/<name>`. Use `-maxdepth 3` for `find`:

```bash
# logoscore CLI (NOT the old liblogos build binary)
find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f 2>/dev/null | head -1

# logos_host
find /nix/store -maxdepth 3 -name logos_host -path "*liblogos-bin*" -type f 2>/dev/null | head -1

# lgpm
find /nix/store -maxdepth 3 -name lgpm -path "*/bin/*" -type f 2>/dev/null | head -1
```

**Warning:** `-maxdepth 4` scans 28K+ entries in WSL and takes ~14 seconds. `-maxdepth 2` misses binaries. `-maxdepth 3` is the correct depth.

## LLM Integration

### Provider configuration

API keys must be passed to the module process, not just set in the calling process. Use `metaConfigure`:

```bash
logoscore call pilot metaConfigure llm.provider anthropic
logoscore call pilot metaConfigure llm.api_key sk-ant-...
logoscore call pilot metaConfigure llm.model claude-sonnet-4-6-20250514
```

The module stores these in SQLite and restores them on restart via `loadIdentity()`.

### Supported providers

All providers except Anthropic route through the OpenAI-compatible chat completions endpoint:

| Provider | Base URL | Default model | Auth |
|----------|----------|---------------|------|
| Anthropic | `api.anthropic.com` (native) | claude-sonnet-4-6 | `x-api-key` header |
| OpenAI | `api.openai.com/v1` | gpt-4o | `Bearer` token |
| DeepSeek | `api.deepseek.com` (no `/v1`) | deepseek-chat | `Bearer` token |
| Google Gemini | `generativelanguage.googleapis.com/v1beta/openai` | gemini-2.5-flash | `Bearer` token |
| OpenRouter | `openrouter.ai/api/v1` | anthropic/claude-sonnet-4-6 | `Bearer` token |
| Groq | `api.groq.com/openai/v1` | llama-3.3-70b-versatile | `Bearer` token |

**Google Gemini gotcha:** API keys from Google Cloud Console may not work. Use keys from **aistudio.google.com** which work immediately with the Generative Language API.

### Cross-process environment variables

`putEnv()` in the CLI process does NOT affect the `logos_host_qt` module process. To pass env vars to the module, use `metaConfigure("llm.api_key", key)` which calls `setenv()` inside the module process and stores the key in SQLite for restart persistence.

## Lazy Dependency Initialization

`initDependencyModules()` configures storage_module (`init` + `start`) and delivery_module (`createNode` + `start`). This can block for 30+ seconds (delivery NAT detection + peer dialing).

**Do NOT call in `initialize()`.** Identity creation only needs the wallet module. Call `initDependencyModules()` lazily on first use:

```cpp
std::string PilotImpl::storageUpload(...) {
    if (!logosAPI_ || !db_) return "{\"error\": \"not initialized\"}";
    initDependencyModules();  // runs once, skips if depsInitialized_
    // ... rest of method
}
```

Methods that need lazy init: `storageUpload`, `storageDownload`, `storageShare`, `messagingSend`, `establishOwnerChannel`.

The `depsInitialized_` flag ensures it only runs once. Subsequent calls skip immediately.

## Running Two Agents Locally

Two agents on the same machine require network isolation because:
- `storage_module` uses a global LevelDB cache (`~/.cache/storage/`) — only one instance per machine
- `delivery_module` binds TCP port 60000 — two Core-mode nodes conflict

### Docker approach (recommended)

Agent A runs on the host. Agent B runs in a Docker container with its own network namespace.

```bash
# Prepare modules for Agent B (no storage_module)
MODULES_B=/tmp/pilot-logoscore/modules-b
mkdir -p $MODULES_B
for m in capability_module lez_wallet_module delivery_module chat_module pilot; do
  cp -r /tmp/pilot-logoscore/modules/$m $MODULES_B/$m
done

# Start Agent B in Docker
docker run --rm -d \
  --name pilot-agent-b \
  -v /nix/store:/nix/store:ro \
  -v $MODULES_B:/modules:ro \
  -v /tmp/agent-b:/data \
  -e LOGOS_HOST_PATH=$(find /nix/store -maxdepth 3 -name logos_host -path "*liblogos-bin*" -type f | head -1) \
  -e PILOT_SEQUENCER_ADDR=http://host.docker.internal:3040 \
  -e PILOT_WAKU_ADDR=/ip4/host.docker.internal/tcp/30303 \
  --add-host=host.docker.internal:host-gateway \
  ubuntu:22.04 \
  $(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f | head -1) \
  --config-dir /data/.logoscore -D -m /modules

# Load modules
LOGOSCORE=$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f | head -1)
for m in capability_module lez_wallet_module delivery_module pilot; do
  docker exec pilot-agent-b $LOGOSCORE --config-dir /data/.logoscore load-module $m
done

# Initialize
docker exec pilot-agent-b $LOGOSCORE --config-dir /data/.logoscore call pilot initialize /data

# Interact
docker exec pilot-agent-b $LOGOSCORE --config-dir /data/.logoscore call pilot getAgentNpk
```

Key points:
- `/nix/store` mounted read-only — no Nix installation needed in container
- `ubuntu:22.04` provides compatible glibc for Nix binaries
- `host.docker.internal` reaches the host's sequencer and Waku node
- Agent B gets its own port 60000 (isolated network namespace)

#### Updated for LEZ v0.1.2 (2026-06-03)

The snippet above is the original shape; on the current stack apply these deltas (all baked into
`test-two-agents-docker.sh`, verified 13/14):

- Module renamed: `lez_wallet_module` → **`logos_execution_zone`** (in both the copy loop and load loop).
- Sequencer port is **3040**, not 8080: `PILOT_SEQUENCER_ADDR=http://host.docker.internal:3040`.
- **The container MUST get the RISC0 environment** or Agent B's wallet/identity init dies and *every*
  B call returns an empty response (which then shows up on A's side as
  `encryption failed: invalid recipient key: not a hex string`, because B never produced keys). Add:
  ```bash
    -v /home/$USER/.risc0:/root/.risc0:ro \
    -e RISC0_DEV_MODE=1 \
    -e LOGOS_BLOCKCHAIN_CIRCUITS=<logos-blockchain-circuits store path> \
    -e PATH=/root/.risc0/extensions/<r0vm-version>:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  ```
- **Cross-agent payment uses the recipient's keys, not its account id:** `walletSend <B_npk_json> <amt>`
  (a bare account id only works for accounts the *sender's own* wallet owns — see the wallet API notes).
- B's `initialize` in a cold container (mounted `/nix/store` cold-load + funding ZK proof) can exceed
  **60s** — use a ~120s client call timeout. It's a stopwatch, not a real failure: `getAgentNpk` and all
  later B operations pass, confirming B did initialise.
- One shared **nwaku** serves both agents (`docker compose up -d nwaku`); each agent's `delivery_module`
  runs its own embedded Core-mode Waku node and peers with that nwaku on `:30303`.

### Environment variables for multi-agent

| Variable | Default | Description |
|----------|---------|-------------|
| `PILOT_TCP_PORT` | 60000 | Waku relay TCP port |
| `PILOT_WAKU_MODE` | Core | `Core` (full relay) or `Edge` (lightweight) |
| `PILOT_NAT` | (auto) | NAT config: `extip:127.0.0.1` to skip detection in WSL |
| `PILOT_WAKU_ADDR` | /ip4/127.0.0.1/tcp/30303 | Static Waku peer address |
| `PILOT_SEQUENCER_ADDR` | http://127.0.0.1:3040 | LEZ sequencer endpoint |

### Running the two-agent test

Prerequisites: sequencer running (`./run-sequencer.sh`), Waku node running (`docker-compose up -d`), modules installed (`./setup-modules.sh`).

```bash
# Full automated test
./test-two-agents-docker.sh

# Expected: 14/14 pass
```

## Running Tests

### Test suites

```bash
# Unit tests (44 tests, no runtime needed)
cd pilot-module && nix build .#unit-tests --extra-experimental-features 'nix-command flakes' -L

# Single-agent integration (28 tests, needs sequencer)
./run-sequencer.sh  # in another terminal
./test-phases.sh

# Two-agent integration (14 tests, needs sequencer + Docker)
./test-two-agents-docker.sh
```

## Linking SQLite

If your module uses SQLite, declaring it in `metadata.json` under `find_packages` is NOT enough. You must also link it in CMakeLists.txt:

```cmake
find_package(SQLite3 REQUIRED)

logos_module(
    NAME mymodule
    SOURCES ...
)

target_link_libraries(mymodule_plugin PRIVATE SQLite::SQLite3)
```

Without `target_link_libraries`, the build succeeds but `lm` fails with `undefined symbol: sqlite3_*`.

## Dependency stubs

The code generator creates client stubs for each module in your `dependencies` list. If a dependency module's stub is incompatible with the current SDK version (happened with `waku_module`), remove it from `metadata.json` dependencies and call it at runtime instead:

```cpp
// Works without compile-time stubs
auto* waku = logosAPI_->getClient("waku_module");
waku->invokeRemoteMethod("waku_module", "storeQuery", topic);
```

## Testing

### Unit tests (no runtime needed)

Create `tests/` directory with three files:

```
tests/
├── main.cpp           — #include <logos_test.h> / LOGOS_TEST_MAIN()
├── test_mymodule.cpp  — LOGOS_TEST(name) { ... }
└── CMakeLists.txt     — logos_test() macro
```

Tests instantiate your impl class directly — no logoscore, no Qt runtime, no dependencies needed:

```cpp
LOGOS_TEST(echo_works) {
    MyImpl impl;
    LOGOS_ASSERT_EQ(impl.echo("hello"), std::string("echo: hello"));
}
```

### Verifying with `lm` (module inspector)

```bash
# Find lm in the nix store
find /nix/store -name "lm" -path "*/logos-module*/bin/*" 2>/dev/null

# Inspect your module
lm result/lib/mymodule_plugin.so
```

This shows metadata + all registered methods. If it fails with `undefined symbol`, you're missing a `target_link_libraries`.

### logoscore CLI — headless runtime

**Source:** https://github.com/logos-co/logos-logoscore-cli
**Library:** https://github.com/logos-co/logos-liblogos

There are two separate things called "logoscore":
- **`logos-logoscore-cli`** — the real CLI tool with `-c`, `-l`, `-m`, daemon mode. This is what you want.
- **`logos-liblogos-build`** — the core C library (`liblogos_core.so` + `logos_host`). This is what `logos-module-builder` pulls in as a build dependency. It has NO CLI argument parsing.

Always build the CLI from its own repo:

```bash
nix build 'github:logos-co/logos-logoscore-cli' --extra-experimental-features 'nix-command flakes'
```

#### Inline mode — quick method calls

```bash
logoscore -m ./result/lib -l pilot -c "pilot.echo(hello)" --quit-on-finish
logoscore -m ./result/lib -l pilot -c "pilot.metaSkills()" --quit-on-finish
logoscore -m ./result/lib -l pilot,chat_module -c "pilot.echo(test)"
```

Flags:
- `-m, --modules-dir <path>` — where to find `.so` files (repeatable for multiple dirs)
- `-l, --load-modules <names>` — comma-separated module names to load
- `-c, --call <call>` — method invocation: `module.method(arg1, arg2)` (repeatable)
- `--quit-on-finish` — exit after all `-c` calls complete
- `--persistence-path <path>` — module data directory (default: `~/.logoscore/data`)

#### Daemon mode — persistent runtime for agent operation

```bash
# Start daemon with modules directory
logoscore -D -m ./modules

# In another terminal — interact with the running daemon
logoscore status                              # check daemon state
logoscore load-module pilot                   # load a module
logoscore list-modules --loaded               # see what's running
logoscore module-info pilot                   # inspect methods
logoscore call pilot echo "hello world"       # call a method
logoscore call pilot metaSkills               # no-arg method
logoscore call storage load_config @config.json  # @file reads file content
logoscore watch pilot --event testEvent --json   # stream events (Ctrl+C to stop)
logoscore stop                                # shutdown daemon
```

Daemon state lives under `~/.logoscore/` (override with `--config-dir`):
- `daemon/state.json` — live state (created at boot, removed at shutdown)
- `daemon/tokens.json` — auth tokens (hashed at rest)
- `client/auto.json` — auto-issued local client token

#### Authentication

Local same-host connections are automatic — the daemon issues an `auto` token at boot. For remote access:

```bash
logoscore issue-token --name alice             # create named token
logoscore issue-token --name ci --expires 30d  # with expiration
logoscore list-tokens                          # see all tokens
logoscore revoke-token alice                   # revoke
```

#### Parallel instances

```bash
logoscore --config-dir /tmp/agent-a -D -m ./modules &
logoscore --config-dir /tmp/agent-b -D -m ./modules &
logoscore --config-dir /tmp/agent-a call pilot echo "I am agent A"
logoscore --config-dir /tmp/agent-b call pilot echo "I am agent B"
```

#### Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error / daemon not running |
| 2 | No daemon running |
| 3 | Module error (not found, load/unload failed) |
| 4 | Method error (not found, call failed, timeout) |

### Testing with Basecamp (GUI)

**Source:** https://github.com/logos-co/logos-basecamp (release 0.1.2-RC3)

For full integration testing with the QML UI:

```bash
BASECAMP_BIN=$(nix build "github:logos-co/logos-basecamp" \
  --extra-experimental-features 'nix-command flakes' \
  --no-link --print-out-paths)/bin/LogosBasecamp

mkdir -p /tmp/pilot-basecamp/modules
cp result/lib/pilot_plugin.so /tmp/pilot-basecamp/modules/

export QT_QPA_PLATFORM=xcb  # for WSL/headless Linux
$BASECAMP_BIN --user-dir /tmp/pilot-basecamp
```

The `--user-dir` flag isolates the instance with its own `modules/`, `plugins/`, `module_data/`, and `logs/` directories.

### What works at each testing level

| Level | What works | What doesn't |
|-------|-----------|-------------|
| Unit tests (`nix build .#unit-tests`) | All 44 tests, crypto, skills, LLM, core | No inter-module calls |
| logoscore inline (`-c ... --quit-on-finish`) | Load module, call methods, scripting | No dependency modules unless also loaded with `-l` |
| logoscore daemon (`-D`) | Persistent runtime, load/unload, events, auth | No GUI |
| Basecamp (`--user-dir`) | Everything — LogosAPI, inter-module, QML UI | Needs display or `QT_QPA_PLATFORM=offscreen` |

## Installing into Logos Basecamp

### Where Basecamp stores things

```
~/.local/share/Logos/LogosBasecamp/
├── modules/     — core modules (.so files)
├── plugins/     — UI plugins (QML or .so)
└── module_data/ — per-module persistent data
```

### Portable vs non-portable builds

**This is critical.** A regular `nix build` produces a `.so` with hardcoded `/nix/store/...` paths for Qt and other libraries. This works inside the nix environment but **breaks inside Basecamp** because Basecamp bundles its own Qt.

```bash
# WRONG for Basecamp — hardcoded nix paths
nix build
ldd result/lib/mymodule_plugin.so
# libQt6Core.so.6 => /nix/store/xxx-qtbase-6.9.2/lib/libQt6Core.so.6  ← BAD

# RIGHT for Basecamp — portable, uses runtime libraries
nix build .#lgx-portable
# or
nix build .#install-portable
```

The portable build sets `RUNPATH` to `$ORIGIN/.` and bundles dependency libraries (libssl, libcrypto, libboost) alongside the `.so`. Qt libraries are resolved from Basecamp's runtime.

### SQLite must be bundled manually

The portable build does NOT include `libsqlite3.so`. If your module uses SQLite, you must copy it manually after installing:

```bash
chmod -R u+w ~/.local/share/Logos/LogosBasecamp/modules/mymodule/
cp /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 \
   ~/.local/share/Logos/LogosBasecamp/modules/mymodule/libsqlite3.so
```

Without this, Basecamp logs: `Failed to load plugin: libsqlite3.so: cannot open shared object file`

### Install a core module via lgpm

```bash
# Find lgpm in nix store
LGPM=$(find /nix/store -name "lgpm" -path "*/bin/*" 2>/dev/null | head -1)

# Build the portable .lgx package
nix --extra-experimental-features "nix-command flakes" build .#lgx-portable

# Install into Basecamp
$LGPM install --file result/logos-mymodule-lib.lgx \
  --modules-dir ~/.local/share/Logos/LogosBasecamp/modules \
  --allow-unsigned

# Don't forget to bundle sqlite if needed (see above)
```

### Manifest format matters

Basecamp v0.1.1 uses `manifestVersion: "0.1.0"`. Basecamp v0.1.2+ uses `manifestVersion: "0.2.0"` with a `"view"` field for QML plugins. Check which version you're targeting.

**Core module manifest (v0.1.0):**
```json
{
  "name": "mymodule",
  "type": "core",
  "main": { "linux-amd64": "mymodule_plugin.so" },
  "manifestVersion": "0.1.0",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "My module",
  "dependencies": ["delivery_module", "storage_module"],
  "category": "general"
}
```

**Core module manifest (v0.2.0):**
```json
{
  "name": "mymodule",
  "type": "core",
  "main": { "linux-amd64": "mymodule_plugin.so" },
  "manifestVersion": "0.2.0",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "My module",
  "dependencies": ["delivery_module", "storage_module"],
  "category": "general"
}
```

### Platform key must match

The nix build produces `"linux-amd64-dev"` as the platform key. Basecamp expects `"linux-amd64"`. If your module doesn't load, check the manifest:

```bash
# WRONG
"main": { "linux-amd64-dev": "mymodule_plugin.so" }

# RIGHT
"main": { "linux-amd64": "mymodule_plugin.so" }
```

### Dependency names must match installed modules

List what's actually installed before declaring dependencies:

```bash
ls ~/.local/share/Logos/LogosBasecamp/modules/
```

Use those exact names. Common mismatches:
- `lez_wallet_module` (in spec) vs `liblogos_execution_zone_wallet_module` (installed name)
- `wallet_module` vs `lez_wallet_module` — different modules entirely

### Installing dependency modules from nix store

If the package manager isn't working, you can install dependency modules directly from your nix store (they're cached from your build):

```bash
MODULES_DIR=~/.local/share/Logos/LogosBasecamp/modules
for mod in delivery_module chat_module storage_module; do
    src=$(find /nix/store -maxdepth 1 -name "*logos-${mod}-module" -type d 2>/dev/null | head -1)
    if [ -d "$src/lib" ]; then
        mkdir -p "$MODULES_DIR/$mod"
        cp "$src/lib/"* "$MODULES_DIR/$mod/"
        # Create a manifest.json for each (see format above)
    fi
done
```

### Create a QML UI plugin

Core modules and UI plugins are **always separate** — different folders, different packages. You cannot bundle them in one `.lgx`.

Basecamp UI plugins go in `~/.local/share/Logos/LogosBasecamp/plugins/myapp/`:

```
myapp/
├── manifest.json
└── Main.qml
```

manifest.json (v0.2.0 — for Basecamp 0.1.2+):
```json
{
  "name": "myapp",
  "type": "ui_qml",
  "main": {},
  "view": "Main.qml",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "My Logos app",
  "dependencies": ["my_core_module"],
  "category": "general",
  "manifestVersion": "0.2.0"
}
```

manifest.json (v0.1.0 — for Basecamp 0.1.1):
```json
{
  "name": "myapp",
  "type": "ui_qml",
  "main": { "linux-amd64": "Main.qml" },
  "version": "1.0.0",
  "author": "Your Name",
  "description": "My Logos app",
  "dependencies": ["my_core_module"],
  "category": "general",
  "manifestVersion": "0.1.0"
}
```

### Calling core module methods from QML

The QML bridge provides `logos.callModule(moduleName, methodName, args)`:

```javascript
// In your Main.qml
function call(method, args) {
    if (typeof logos !== "undefined" && logos.callModule)
        return logos.callModule("mymodule", method, args)
    return "Bridge unavailable"
}

// Usage
Button {
    text: "Echo"
    onClicked: result = call("echo", ["hello"])
}
```

Returns the method's return value as a string, or `"false"` if the call failed.

### QML syntax rules

Do NOT put semicolons between QML property assignments in one-liner components:

```qml
// WRONG — "Unexpected token ';'"
Button { text: "Go"; onClicked: doThing(); background: Rectangle { color: "red" } }

// RIGHT — expanded
Button {
    text: "Go"
    onClicked: doThing()
    background: Rectangle { color: "red" }
}
```

### Debugging module loading

Check Basecamp's stderr for loading messages:

```bash
~/apps/logos-basecamp-x86_64.AppImage 2>&1 | grep -i "pilot\|error\|fail"
```

Key messages:
- `"Successfully loaded core module: pilot"` — module loaded
- `"Module process crashed: pilot"` — missing library (check ldd)
- `"Failed to load plugin: libsqlite3.so: cannot open"` — bundle sqlite
- `"Token server started, waiting for auth token"` — module waiting for capability_module
- `"Missing dependencies detected"` — dependency name mismatch in manifest

### Verifying module is running

```bash
# Check if logos_host started for your module
ps aux | grep "logos_host.*mymodule" | grep -v grep

# Check IPC sockets exist
ls /tmp/logos_mymodule_*
```

Restart Basecamp after any changes to modules or plugins.

## Common rejection reasons (from lambda-prize)

Based on reviewing rejected submissions:

1. **Not a real Logos module** — building a standalone Python/Rust app instead of a `.so` that loads into logoscore. The module MUST use `logos-module-builder` and compile via `nix build`.

2. **No `module.json` / not a Basecamp app** — the spec says "Logos Basecamp app" meaning Qt/QML, not a web app.

3. **Tests don't pass** — pinning wrong dependency versions, untested code.

4. **Not deployed to LEZ** — no evidence of running against a real sequencer.

5. **Missing demo** — no video, no `demo.sh`, program IDs listed as "[to be added]".

6. **Submitting too fast** — max 1 submission per week, max 3 total per prize.

## Two GitHub orgs

Logos modules are split across two orgs:

- **logos-co** — most modules (delivery, storage, waku, chat, basecamp)
- **logos-blockchain** — LEZ wallet module, key_protocol, execution zone

If you can't find a module in `logos-co`, check `logos-blockchain`.

## Useful nix store paths

After building, these are cached and reusable:

```bash
# Find any header or binary
find /nix/store -name "logos_api.h" 2>/dev/null
find /nix/store -name "logoscore" -type f 2>/dev/null
find /nix/store -name "lm" -path "*/bin/*" 2>/dev/null
find /nix/store -name "lgpm" -path "*/bin/*" 2>/dev/null
```

The entire nix store is shared across all Logos projects. Building one module caches dependencies for all future modules.
