# Weather Skill — Example Third-Party Pilot Skill

Demonstrates how to build a custom skill for the Pilot agent — and ships as a
**real, runtime-loadable plugin** you can build and drop into the agent's
plugins directory without recompiling the core module.

Files in this directory:

| File | Purpose |
|------|---------|
| `weather_skill.h` / `weather_skill.cpp` | The skill itself — a pure `PilotSkill` (no Qt). |
| `weather_plugin.cpp` | Qt plugin wrapper (`Q_PLUGIN_METADATA` + the Pilot IID) that hands `WeatherSkill` to the agent. |
| `weather_plugin.json` | Plugin metadata embedded for operator audit. |
| `CMakeLists.txt` | Self-contained build that produces `libweather_plugin.so`. |

## Implement the PilotSkill interface

```cpp
#include "pilot_skill.h"

class WeatherSkill : public PilotSkill {
public:
    std::string name() const override { return "weather.lookup"; }
    std::string category() const override { return "utility"; }
    std::string description() const override { return "Returns weather data"; }
    std::string inputSchema() const override { return "{...}"; }
    std::string outputSchema() const override { return "{...}"; }
    int64_t priceLez() const override { return 1; }
    std::string execute(const std::string& argsJson) override {
        // Parse argsJson, do work, return JSON result
    }
};
```

## Register the skill

There are two ways to register a skill. Both use the same stable `PilotSkill`
interface, so this skill code is drop-in either way.

### Option A — runtime plugin (no core rebuild)

A third party can add this skill **without modifying or recompiling the core module**.
The skill is wrapped in a `PilotPluginInterface` Qt plugin (`weather_plugin.cpp`),
built into a `.so`, and dropped in the operator's plugins directory. The wrapper is
small — it just declares the IID and hands `WeatherSkill` over:

```cpp
// weather_plugin.cpp (abridged — see the real file)
class WeatherPlugin : public QObject, public PilotPluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PilotPluginInterface_iid FILE "weather_plugin.json")
    Q_INTERFACES(PilotPluginInterface)
public:
    int abiVersion() const override { return PILOT_PLUGIN_ABI_VERSION; }
    std::string pluginName() const override { return "weather"; }
    std::string pluginVersion() const override { return "1.0.0"; }
    std::vector<std::unique_ptr<PilotSkill>> createSkills() override {
        std::vector<std::unique_ptr<PilotSkill>> skills;
        skills.push_back(std::make_unique<WeatherSkill>());
        return skills;
    }
};
```

Build the plugin (self-contained — needs only the two stable headers + QtCore):

```bash
# Standalone, pointing at the public headers:
cmake -S examples/skill-weather -B build-weather \
      -DPILOT_SRC_DIR=$PWD/pilot-module/src
cmake --build build-weather
# -> build-weather/libweather_plugin.so

# Or as part of the module build:
cmake -S pilot-module -B build -DPILOT_BUILD_EXAMPLES=ON
cmake --build build --target weather_plugin
```

Then enable loading and place the plugin:

```bash
export PILOT_ENABLE_PLUGINS=1            # OFF by default; opt-in required
mkdir -p ~/.pilot/plugins               # or set PILOT_PLUGINS_DIR
cp build-weather/libweather_plugin.so ~/.pilot/plugins/
```

> **TRUST — this is NOT a sandbox.** A plugin is native code loaded into the agent
> process, which holds the operator's keys and funds. Anything you place in the plugins
> directory runs with the **full privileges of the agent**. The directory is an
> operator-trusted boundary, not a security boundary. The loader isolates *failures*
> (a plugin that fails to load, mismatches the ABI, throws, or clashes with an existing
> skill name is logged and skipped — never crashing the module, never shadowing a
> built-in), but it provides **no runtime isolation** of a plugin's behavior.

See `docs/skill-interface.md` (_Runtime Plugins_) and
`pilot-module/src/pilot_plugin_iface.h` for the full ABI contract.

### Option B — built-in (compiled into the module)

Add the skill in `pilot-module/src/pilot_builtin_skills.cpp`, alongside the existing
registrations, and rebuild:

```cpp
registry.registerSkill(std::make_unique<WeatherSkill>());
```

```bash
cd pilot-module && nix build
```

## Verify it is registered

```bash
# daemon mode (see project README for setup).
# For Option A, the agent must be started with PILOT_ENABLE_PLUGINS=1 and the
# .so present in the plugins dir; the loader registers it during module init.
logoscore call pilot metaSkills        # weather.lookup appears in the list
```

If a plugin fails to load (wrong IID, ABI mismatch, throws, or its skill name
clashes with an existing one) it is logged and skipped — it will **not** appear
in `metaSkills` and will **not** crash the agent. Check the agent log for
`[pilot] plugins: SKIP ...` lines.

Built-in skills are invoked through their provider methods and over the A2A task
interface; see `docs/skill-interface.md` for the dispatch contract.

## This example is also a loader test

The module test suite (`pilot-module/tests/test_skills.cpp`, via
`pilot-module/tests/CMakeLists.txt`) compiles **this exact plugin** into a real `.so`
and drives the loader against it: with `PILOT_ENABLE_PLUGINS=1` it asserts
`weather.lookup` both registers and dispatches, and that a name clash with an existing
skill is skipped rather than allowed to shadow it. The same suite verifies the loader is
inert when the env var is unset and that a malformed library is skipped without crashing.
So the runtime-plugin path here is exercised, not just described.
