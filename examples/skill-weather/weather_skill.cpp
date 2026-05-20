#include "weather_skill.h"

std::string WeatherSkill::execute(const std::string& argsJson) {
    std::string location = "Lagos";
    size_t pos = argsJson.find("\"location\"");
    if (pos != std::string::npos) {
        size_t valStart = argsJson.find(':', pos) + 1;
        size_t qStart = argsJson.find('"', valStart) + 1;
        size_t qEnd = argsJson.find('"', qStart);
        if (qEnd != std::string::npos)
            location = argsJson.substr(qStart, qEnd - qStart);
    }

    return "{\"location\": \"" + location + "\", \"temp_c\": 28, "
           "\"condition\": \"partly_cloudy\", \"humidity\": 72}";
}
