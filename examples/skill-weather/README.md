# Weather Skill — Example Third-Party Pilot Skill

Demonstrates how to build a custom skill for the Pilot agent.

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

## Build as shared library

```cmake
add_library(weather_skill SHARED weather_skill.cpp)
target_include_directories(weather_skill PRIVATE /path/to/pilot-module/src)
```

## Install

Copy the `.so` to the Pilot agent's plugins directory:

```bash
cp libweather_skill.so ~/.pilot/plugins/
```

The agent loads all `.so` files from the plugins directory on startup.

## Test via A2A

```bash
logoscore -m ./result/lib -l pilot \
  -c "pilot.dispatchSkill(weather.lookup, {\"location\": \"London\"})"
```
