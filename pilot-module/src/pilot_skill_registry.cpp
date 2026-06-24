#include "pilot_skill.h"
#include "pilot_plugin_iface.h"
#include <exception>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QDir>
#include <QFileInfoList>
#include <QLibrary>
#include <QPluginLoader>
#include <QObject>
#include <QDebug>

void SkillRegistry::registerSkill(std::unique_ptr<PilotSkill> skill) {
    std::string n = skill->name();
    skills_[n] = std::move(skill);
}

namespace {
// Trust gate: plugin loading is OFF BY DEFAULT. Enabled only when PILOT_ENABLE_PLUGINS
// is present and not an explicit "off" value. Anything else (unset/empty/"0"/"false"/"no")
// leaves the loader inert so behavior is unchanged from a build without it.
bool pluginsEnabled() {
    const char* v = std::getenv("PILOT_ENABLE_PLUGINS");
    if (!v) return false;
    // Fail CLOSED: enable native-code plugin loading ONLY for an explicit truthy token
    // ("1"/"true"/"yes"/"on", case-insensitive, whitespace-trimmed). Every other value —
    // including "off"/"false"/"disabled"/unset/unrecognized — leaves the loader OFF. A
    // blocklist can never be complete for a security toggle on a key+money-holding process,
    // so "off"/"False" must NOT accidentally turn it on.
    std::string s(v);
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    s = s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}
}  // namespace

void SkillRegistry::loadPlugins(const std::string& dir) {
    // Default-off trust gate. No env -> no scan, no .so opened, no behavior change.
    if (!pluginsEnabled()) return;

    QDir pluginsDir(QString::fromStdString(dir));
    if (!pluginsDir.exists()) {
        qInfo() << "[pilot] plugins: directory does not exist, nothing to load:"
                << QString::fromStdString(dir);
        return;
    }

    qInfo() << "[pilot] plugins: ENABLED. Scanning operator-trusted directory"
            << QString::fromStdString(dir)
            << "- loaded plugins run with FULL agent privileges (no sandbox).";

    const QFileInfoList entries = pluginsDir.entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo& fi : entries) {
        const QString path = fi.absoluteFilePath();
        // Only consider real shared libraries (.so/.dylib/.dll). Skip everything else
        // (READMEs, configs) silently — they are not plugin candidates.
        if (!QLibrary::isLibrary(path)) continue;

        // Per-plugin isolation: any failure here is logged + skipped and must never
        // crash the module nor be mistaken for a successful load.
        try {
            QPluginLoader loader(path);
            QObject* root = loader.instance();
            if (!root) {
                qWarning() << "[pilot] plugins: SKIP (failed to load)" << path
                           << "-" << loader.errorString();
                continue;
            }

            // IID gate (first line of defense): the object must implement OUR interface.
            // A library that is a Qt plugin for something else casts to null here.
            auto* plugin = qobject_cast<PilotPluginInterface*>(root);
            if (!plugin) {
                qWarning() << "[pilot] plugins: SKIP (not a Pilot plugin / wrong IID)"
                           << path;
                loader.unload();   // took nothing from it; release the library
                continue;
            }

            // ABI handshake: refuse a plugin compiled against an incompatible interface
            // rather than risk a vtable-layout mismatch when we call into its skills.
            if (plugin->abiVersion() != PILOT_PLUGIN_ABI_VERSION) {
                qWarning() << "[pilot] plugins: SKIP (ABI mismatch: plugin reports"
                           << plugin->abiVersion() << "expected" << PILOT_PLUGIN_ABI_VERSION
                           << ")" << path;
                loader.unload();
                continue;
            }

            const QString pname = QString::fromStdString(plugin->pluginName());
            const QString pver  = QString::fromStdString(plugin->pluginVersion());

            // createSkills() is third-party code: it may throw. Contain it.
            std::vector<std::unique_ptr<PilotSkill>> skills = plugin->createSkills();

            int registered = 0;
            for (auto& skill : skills) {
                if (!skill) continue;   // a null entry is ignored, not fatal
                const std::string name = skill->name();
                // Never overwrite an existing skill (builtin or earlier plugin). On a
                // name clash we keep the incumbent and skip the newcomer — a plugin must
                // not be able to shadow wallet.send etc.
                if (hasSkill(name)) {
                    qWarning() << "[pilot] plugins: SKIP skill (name already registered)"
                               << QString::fromStdString(name) << "from" << path;
                    continue;
                }
                registerSkill(std::move(skill));
                ++registered;
            }

            // IMPORTANT: do NOT unload() on the success path. The registered skills'
            // code + vtables live inside this .so; unloading would dangle them. Letting
            // `loader` go out of scope keeps the library mapped (Qt refcounts loaded
            // plugins and does not unload on destruction).
            qInfo() << "[pilot] plugins: loaded" << path
                    << "(name:" << (pname.isEmpty() ? QString("<unnamed>") : pname)
                    << "version:" << (pver.isEmpty() ? QString("<none>") : pver)
                    << ") - registered" << registered << "skill(s)";
        } catch (const std::exception& e) {
            qWarning() << "[pilot] plugins: SKIP (threw during load)" << path
                       << "-" << e.what();
        } catch (...) {
            qWarning() << "[pilot] plugins: SKIP (threw unknown exception during load)"
                       << path;
        }
    }
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
    // Skill-failure isolation (spec Reliability #3): a throwing skill must not unwind
    // out of the Qt slot and std::terminate the whole module. Convert any exception
    // into a JSON error so the module survives and other skills keep running.
    try {
        return it->second->execute(argsJson);
    } catch (const std::exception& e) {
        QJsonObject err;
        err["error"] = QString::fromStdString("skill '" + name + "' failed: " + e.what());
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    } catch (...) {
        QJsonObject err;
        err["error"] = QString::fromStdString("skill '" + name + "' failed with an unknown error");
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }
}

bool SkillRegistry::hasSkill(const std::string& name) const {
    return skills_.find(name) != skills_.end();
}

size_t SkillRegistry::count() const {
    return skills_.size();
}
