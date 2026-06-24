// weather_plugin.cpp — Real, loadable Pilot skill plugin (Usability #1).
// ---------------------------------------------------------------------------
// This is the runtime-plugin half of the example. It wraps the plain
// WeatherSkill (weather_skill.h, a pure PilotSkill) in a Qt plugin that the
// core agent discovers with QPluginLoader at runtime — NO core rebuild needed.
//
// Build it as a shared library (see CMakeLists.txt here), then:
//
//   export PILOT_ENABLE_PLUGINS=1        # OFF by default; opt-in required
//   mkdir -p ~/.pilot/plugins            # or set PILOT_PLUGINS_DIR
//   cp libweather_plugin.so ~/.pilot/plugins/
//
// TRUST: anything dropped in the plugins directory runs with the FULL
// privileges of the agent (keys, funds, DB, network). The directory is an
// operator-trusted boundary, NOT a sandbox. See pilot_plugin_iface.h.
//
// This translation unit depends ONLY on the two stable headers a third party
// is meant to use — pilot_skill.h and pilot_plugin_iface.h — plus QtCore. It
// pulls in nothing from the core module's internals.
//
#include "pilot_plugin_iface.h"   // PilotPluginInterface (+ pilot_skill.h)
#include "weather_skill.h"        // WeatherSkill : public PilotSkill

#include <QObject>

#include <memory>
#include <string>
#include <vector>

class WeatherPlugin : public QObject, public PilotPluginInterface {
    Q_OBJECT
    // IID gate: the loader requires an exact match before it instantiates us.
    // FILE embeds human-readable metadata for operator audit (qtdiag, strings).
    Q_PLUGIN_METADATA(IID PilotPluginInterface_iid FILE "weather_plugin.json")
    Q_INTERFACES(PilotPluginInterface)

public:
    // ABI handshake. MUST be the value we compiled against so the loader can
    // reject us on an incompatible interface change instead of risking a crash.
    int abiVersion() const override { return PILOT_PLUGIN_ABI_VERSION; }

    std::string pluginName() const override { return "weather"; }
    std::string pluginVersion() const override { return "1.0.0"; }

    // Factory: hand the agent the skills this plugin provides. Ownership of each
    // PilotSkill transfers to the agent's SkillRegistry, which destroys it on
    // shutdown. We keep no ownership.
    std::vector<std::unique_ptr<PilotSkill>> createSkills() override {
        std::vector<std::unique_ptr<PilotSkill>> skills;
        skills.push_back(std::make_unique<WeatherSkill>());
        return skills;
    }
};

// Q_OBJECT in a .cpp: AUTOMOC generates this and it must be included here.
#include "weather_plugin.moc"
