#pragma once
//
// pilot_plugin_iface.h — Stable plugin ABI for third-party Pilot skills.
// ---------------------------------------------------------------------------
// This is the ONE header a third party links against to ship skills WITHOUT
// modifying the core agent module (Usability #1). A plugin author:
//
//   1. Implements one or more PilotSkill subclasses (see pilot_skill.h).
//   2. Implements a PilotPluginInterface whose createSkills() hands those
//      skills to the agent.
//   3. Exports it as a Qt plugin (Q_PLUGIN_METADATA + Q_INTERFACES), builds a
//      shared library, and drops it in the operator's plugins directory.
//
// The core module discovers such libraries with QPluginLoader at runtime
// (see the loader in pilot_skill_registry.cpp — SkillRegistry::loadPlugins).
// NOTHING in core needs to change to add a skill.
//
// =====================  TRUST MODEL — READ THIS  ===========================
// A plugin is a NATIVE shared object loaded into the agent process. The agent
// holds the operator's keys and funds. A loaded plugin therefore runs with the
// FULL privileges of the agent — it can read the wallet, the SQLite database,
// the filesystem, and the network. THIS IS NOT A SANDBOX.
//
// Consequences, enforced by the loader (not by this header):
//   * Plugin loading is OFF BY DEFAULT. It only happens when the operator sets
//     PILOT_ENABLE_PLUGINS=1.
//   * Plugins are loaded only from an operator-trusted directory
//     (default ~/.pilot/plugins, override with PILOT_PLUGINS_DIR). Placing a
//     file there is an explicit statement of trust by the operator — it is an
//     operator boundary, NOT a security boundary.
//   * A plugin that fails to load, has the wrong IID, or reports a mismatched
//     ABI version is logged and SKIPPED. It must never be silently treated as
//     loaded.
//
// Do not read this header as a promise of isolation. There is none.
// ===========================================================================
//
// ----------------------------  ABI STABILITY  ------------------------------
// The contract is intentionally tiny so it can stay binary-stable:
//   * It references only PilotSkill (pilot_skill.h) and standard-library /
//     Qt plugin types — no PilotImpl, no module internals.
//   * Ownership transfers by std::unique_ptr<PilotSkill>: the agent owns every
//     returned skill and destroys it (via PilotSkill's virtual dtor) on
//     shutdown. The plugin keeps no ownership.
//   * PILOT_PLUGIN_ABI_VERSION is bumped ONLY on an incompatible change to
//     this interface or to PilotSkill's vtable. The loader compares the
//     plugin's abiVersion() against its own compiled-in value and rejects a
//     mismatch, rather than risking a vtable-layout crash.
//
// NOTE: std::vector/std::unique_ptr/std::string cross the .so boundary here.
// That is safe because plugins and core are built with the SAME toolchain and
// C++ standard library under one Nix toolchain. A plugin built with a
// different/incompatible libstdc++ is out of contract; the abiVersion() check
// guards intentional interface changes, not toolchain mismatches.
//
#include "pilot_skill.h"

#include <vector>
#include <memory>
#include <string>

#include <QtPlugin>

// Bump ONLY on an incompatible change to PilotPluginInterface or to the
// PilotSkill vtable. The loader rejects plugins whose abiVersion() differs.
#define PILOT_PLUGIN_ABI_VERSION 1

// Stable interface contract that a third-party plugin implements.
//
// A plugin's concrete class typically inherits both QObject and this
// interface, e.g.:
//
//   class WeatherPlugin : public QObject, public PilotPluginInterface {
//       Q_OBJECT
//       Q_PLUGIN_METADATA(IID PilotPluginInterface_iid)
//       Q_INTERFACES(PilotPluginInterface)
//   public:
//       int abiVersion() const override { return PILOT_PLUGIN_ABI_VERSION; }
//       std::string pluginName() const override { return "weather"; }
//       std::string pluginVersion() const override { return "1.0.0"; }
//       std::vector<std::unique_ptr<PilotSkill>> createSkills() override {
//           std::vector<std::unique_ptr<PilotSkill>> skills;
//           skills.push_back(std::make_unique<WeatherSkill>());
//           return skills;
//       }
//   };
//
class PilotPluginInterface {
public:
    virtual ~PilotPluginInterface() = default;

    // ABI handshake. MUST return PILOT_PLUGIN_ABI_VERSION (the value the
    // plugin was compiled against). The loader compares this to its own
    // compiled-in PILOT_PLUGIN_ABI_VERSION and refuses to use the plugin's
    // skills on mismatch. Provided non-pure with a default so existing/minimal
    // plugins still satisfy the vtable, but plugins SHOULD override it.
    virtual int abiVersion() const { return PILOT_PLUGIN_ABI_VERSION; }

    // Human-readable identity for logging and operator audit. Optional.
    virtual std::string pluginName() const { return std::string(); }
    virtual std::string pluginVersion() const { return std::string(); }

    // Factory: build and hand over the skills this plugin provides.
    //
    // Ownership of every returned PilotSkill transfers to the agent, which
    // registers them into its SkillRegistry and destroys them on shutdown.
    // Returning an empty vector is valid (no skills contributed). Throwing is
    // permitted; the loader catches it, logs, and skips this plugin.
    virtual std::vector<std::unique_ptr<PilotSkill>> createSkills() = 0;
};

// Qt plugin IID. The trailing "/1" tracks the major ABI generation; the
// loader requires an exact IID match before it will even instantiate the
// plugin object (a first-line defense before abiVersion() is consulted).
#define PilotPluginInterface_iid "co.logos.pilot.PilotPluginInterface/1"

Q_DECLARE_INTERFACE(PilotPluginInterface, PilotPluginInterface_iid)
