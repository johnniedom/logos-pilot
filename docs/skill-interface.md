# Pilot Skill Interface

Build custom skills for the Pilot agent without modifying core code.

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

## Building as a Shared Library

```cmake
cmake_minimum_required(VERSION 3.14)
project(MySkill LANGUAGES CXX)

add_library(my_skill SHARED my_skill.cpp)
target_include_directories(my_skill PRIVATE /path/to/pilot-module/src)
set_target_properties(my_skill PROPERTIES PREFIX "")
```

Build:
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

## Installation

Copy the `.so` to the plugins directory:

```bash
mkdir -p ~/.pilot/plugins
cp my_skill.so ~/.pilot/plugins/
```

The agent scans this directory on startup and loads all valid skill libraries.

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
