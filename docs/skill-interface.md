# Pilot Skill Interface

Build custom skills for the Pilot agent by implementing the `PilotSkill` interface
and registering them in the module.

> **Two ways to add a skill:**
> 1. **In-tree (built-in):** register it in `registerBuiltinSkills()` and rebuild the
>    module. Best for skills that ship with Pilot.
> 2. **Runtime plugin (third-party, no core rebuild):** ship a native Qt plugin `.so`
>    and drop it in the operator's plugins directory. The module loads it at startup —
>    see _Runtime Plugins_ below. This is **opt-in and is NOT a sandbox** (a loaded
>    plugin runs with full agent privileges).

## The PilotSkill Abstract Class

```cpp
class PilotSkill {
public:
    virtual ~PilotSkill() = default;
    virtual std::string name() const = 0;        // e.g. "weather.lookup"
    virtual std::string category() const = 0;     // e.g. "utility"
    virtual std::string description() const = 0;
    virtual std::string inputSchema() const = 0;  // JSON Schema
    virtual std::string outputSchema() const = 0; // JSON Schema
    virtual int64_t priceLez() const { return 0; }
    virtual std::string execute(const std::string& argsJson) = 0;
};
```

## Implementing a Skill

1. Include `pilot_skill.h` from the pilot-module source
2. Subclass `PilotSkill` and implement all pure virtual methods
3. `execute()` receives a JSON string of arguments, returns a JSON string result
4. Set `priceLez()` to charge LEZ tokens per invocation (0 = free)

## Input/Output Schemas

Use JSON Schema to describe your skill's interface:

```cpp
std::string inputSchema() const override {
    return R"({
        "type": "object",
        "properties": {
            "location": {"type": "string", "description": "City name or coordinates"}
        },
        "required": ["location"]
    })";
}
```

Schemas are used for:
- A2A Agent Card generation (other agents know how to call your skill)
- Input validation before `execute()` is called
- Documentation generation

## Registering a Skill (in-tree / built-in)

Built-in skills are compiled into the module:

1. Add your `PilotSkill` subclass (or use the provided `LambdaSkill`) under
   `pilot-module/src/`.
2. Register it inside `registerBuiltinSkills()` in `pilot-module/src/pilot_builtin_skills.cpp`:
   ```cpp
   registry.registerSkill(std::make_unique<MySkill>());
   // or, for a quick inline skill:
   registry.registerSkill(std::make_unique<LambdaSkill>(
       "weather.lookup", "utility", "Returns weather for a location",
       /*inputSchema*/  "{}", /*outputSchema*/ "{}", /*priceLez*/ 0,
       [](const std::string& argsJson) { return std::string("{...}"); }));
   ```
3. Rebuild the module:
   ```bash
   cd pilot-module && nix build
   ```

See `examples/skill-weather/` for a complete reference skill.

## Runtime Plugins (third-party, no core rebuild)

A third party can add skills **without modifying or recompiling the core module** by
shipping a native Qt plugin and dropping it in the operator's plugins directory. At
startup the module scans that directory, loads each plugin with `QPluginLoader`, and
registers the skills it provides.

### Authoring a plugin

Implement `PilotPluginInterface` (in `pilot-module/src/pilot_plugin_iface.h`) and export
it as a Qt plugin:

```cpp
#include "pilot_skill.h"
#include "pilot_plugin_iface.h"
#include <QObject>

class WeatherPlugin : public QObject, public PilotPluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PilotPluginInterface_iid)
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

Build it as a shared library against the same Qt + toolchain as the module, then place
the resulting `.so` in the plugins directory.

### Enabling and locating plugins

Plugin loading is **OFF BY DEFAULT**. The module only scans for plugins when the
operator opts in:

- `PILOT_ENABLE_PLUGINS` — set to `1` (any value other than empty/`0`/`false`/`no`) to
  enable scanning. Unset, the loader is completely inert and behavior is identical to a
  build without it.
- `PILOT_PLUGINS_DIR` — directory to scan. Defaults to `~/.pilot/plugins`.

### Trust model (read this — it is NOT a sandbox)

A plugin is **native code loaded into the agent process**, which holds the operator's
keys and funds. A loaded plugin therefore runs with the **full privileges of the agent**
— it can read the wallet, the database, the filesystem, and the network.

- The plugins directory is an **operator-trusted boundary, not a security sandbox**.
  Placing a file there is an explicit statement of trust. **No isolation is implemented
  or claimed.**
- What the loader *does* provide is **robustness isolation**, not security: a plugin that
  fails to load, has the wrong IID, reports a mismatched `PILOT_PLUGIN_ABI_VERSION`,
  throws, or names a skill that already exists is **logged and skipped**. A bad plugin
  never crashes the module, never overwrites a built-in skill (name clashes are skipped,
  not replaced), and is never silently treated as loaded.

See `examples/skill-weather/` for the reference skill and `pilot_plugin_iface.h` for the
full ABI contract.

### The loader is real, and tested

This is not an aspirational interface. `SkillRegistry::loadPlugins()`
(`pilot-module/src/pilot_skill_registry.cpp`) is exercised by unit tests in
`pilot-module/tests/test_skills.cpp`:

- **Off by default** — with `PILOT_ENABLE_PLUGINS` unset (and with an explicit `0`),
  `loadPlugins()` is a no-op: nothing is scanned and the registry is unchanged.
- **Failure isolation** — a malformed file with a library extension is logged and
  skipped; the module does not crash and nothing is registered.
- **Positive path** — the bundled `examples/skill-weather` plugin is compiled into a
  real `.so`, discovered with `PILOT_ENABLE_PLUGINS=1`, and its `weather.lookup` skill
  both **registers** and **dispatches** — with the core module unmodified. (This test
  is skipped only where Qt6 is not directly findable at configure time; the safety
  tests always run.)
- **No shadowing** — a plugin whose skill name collides with an existing/builtin skill
  is skipped, not allowed to overwrite the incumbent.

## A2A Integration

Registered skills automatically appear in the Agent Card:

```json
{
    "id": "weather-lookup",
    "name": "Returns weather data for a given location",
    "description": "Returns weather data for a given location",
    "inputModes": ["application/json"],
    "outputModes": ["application/json"]
}
```

Other agents can invoke your skill via the A2A protocol:
```json
{
    "jsonrpc": "2.0",
    "method": "tasks/send",
    "params": {
        "metadata": {"skill": "weather.lookup"},
        "message": {
            "parts": [{"type": "text", "text": "{\"location\": \"Lagos\"}"}]
        }
    }
}
```

## Pricing

Set `priceLez()` to a non-zero value to charge per invocation. Payment follows the pay-on-acceptance model:

1. Requester sees the price in the Agent Card
2. Task is submitted
3. Agent executes the skill
4. On successful completion, requester pays the listed LEZ amount
5. Payment is verified before results are delivered

## Example

See `examples/skill-weather/` for a complete working example.
