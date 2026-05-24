#include "pilot_skill.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

void SkillRegistry::registerSkill(std::unique_ptr<PilotSkill> skill) {
    std::string n = skill->name();
    skills_[n] = std::move(skill);
}

std::string SkillRegistry::listSkills() const {
    QJsonArray arr;
    for (const auto& [name, skill] : skills_) {
        QJsonObject obj;
        obj["name"] = QString::fromStdString(skill->name());
        obj["category"] = QString::fromStdString(skill->category());
        obj["description"] = QString::fromStdString(skill->description());
        obj["price_lez"] = static_cast<double>(skill->priceLez());
        arr.append(obj);
    }
    QJsonObject root;
    root["skills"] = arr;
    root["count"] = static_cast<int>(skills_.size());
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

std::string SkillRegistry::listSkillsForCard() const {
    QJsonArray arr;
    QJsonArray jsonModes;
    jsonModes.append(QString("application/json"));
    for (const auto& [name, skill] : skills_) {
        std::string id = name;
        for (auto& c : id) if (c == '.') c = '-';
        QJsonObject obj;
        obj["id"] = QString::fromStdString(id);
        obj["name"] = QString::fromStdString(skill->name());
        obj["description"] = QString::fromStdString(skill->description());
        obj["inputModes"] = jsonModes;
        obj["outputModes"] = jsonModes;
        arr.append(obj);
    }
    return QJsonDocument(arr).toJson(QJsonDocument::Compact).toStdString();
}

std::string SkillRegistry::dispatch(const std::string& name, const std::string& argsJson) {
    auto it = skills_.find(name);
    if (it == skills_.end()) {
        QJsonObject err;
        err["error"] = QString::fromStdString("unknown skill: " + name);
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }
    return it->second->execute(argsJson);
}

bool SkillRegistry::hasSkill(const std::string& name) const {
    return skills_.find(name) != skills_.end();
}

size_t SkillRegistry::count() const {
    return skills_.size();
}
