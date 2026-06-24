#pragma once
// Resolved via the PILOT_SRC_DIR include directory the CMake adds, so this example
// builds standalone (copied out of the repo) against just the two stable headers.
#include "pilot_skill.h"

class WeatherSkill : public PilotSkill {
public:
    std::string name() const override { return "weather.lookup"; }
    std::string category() const override { return "utility"; }
    std::string description() const override { return "Returns weather data for a given location"; }
    std::string inputSchema() const override {
        return R"({"type":"object","properties":{"location":{"type":"string"}},"required":["location"]})";
    }
    std::string outputSchema() const override {
        return R"({"type":"object","properties":{"location":{"type":"string"},"temp_c":{"type":"number"},"condition":{"type":"string"}}})";
    }
    int64_t priceLez() const override { return 1; }
    std::string execute(const std::string& argsJson) override;
};
