#include "pilot_skill.h"
#include <sstream>

void SkillRegistry::registerSkill(std::unique_ptr<PilotSkill> skill) {
    std::string n = skill->name();
    skills_[n] = std::move(skill);
}

std::string SkillRegistry::listSkills() const {
    std::ostringstream json;
    json << "{\"skills\": [";
    bool first = true;
    for (const auto& [name, skill] : skills_) {
        if (!first) json << ",";
        first = false;
        json << "{"
             << "\"name\": \"" << skill->name() << "\","
             << "\"category\": \"" << skill->category() << "\","
             << "\"description\": \"" << skill->description() << "\","
             << "\"price_lez\": " << skill->priceLez()
             << "}";
    }
    json << "], \"count\": " << skills_.size() << "}";
    return json.str();
}

std::string SkillRegistry::listSkillsForCard() const {
    std::ostringstream json;
    json << "[";
    bool first = true;
    for (const auto& [name, skill] : skills_) {
        if (!first) json << ",";
        first = false;
        std::string id = name;
        for (auto& c : id) if (c == '.') c = '-';
        json << "{"
             << "\"id\": \"" << id << "\","
             << "\"name\": \"" << skill->description() << "\","
             << "\"description\": \"" << skill->description() << "\","
             << "\"inputModes\": [\"application/json\"],"
             << "\"outputModes\": [\"application/json\"]"
             << "}";
    }
    json << "]";
    return json.str();
}

std::string SkillRegistry::dispatch(const std::string& name, const std::string& argsJson) {
    auto it = skills_.find(name);
    if (it == skills_.end())
        return "{\"error\": \"unknown skill: " + name + "\"}";
    return it->second->execute(argsJson);
}

bool SkillRegistry::hasSkill(const std::string& name) const {
    return skills_.find(name) != skills_.end();
}

size_t SkillRegistry::count() const {
    return skills_.size();
}
