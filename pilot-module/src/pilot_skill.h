#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <cstdint>

class PilotSkill {
public:
    virtual ~PilotSkill() = default;
    virtual std::string name() const = 0;
    virtual std::string category() const = 0;
    virtual std::string description() const = 0;
    virtual std::string inputSchema() const = 0;
    virtual std::string outputSchema() const = 0;
    virtual int64_t priceLez() const { return 0; }
    virtual std::string execute(const std::string& argsJson) = 0;
};

class LambdaSkill : public PilotSkill {
public:
    LambdaSkill(std::string name, std::string category, std::string desc,
                std::string inSchema, std::string outSchema, int64_t price,
                std::function<std::string(const std::string&)> fn)
        : name_(std::move(name)), category_(std::move(category)),
          desc_(std::move(desc)), inSchema_(std::move(inSchema)),
          outSchema_(std::move(outSchema)), price_(price), fn_(std::move(fn)) {}

    std::string name() const override { return name_; }
    std::string category() const override { return category_; }
    std::string description() const override { return desc_; }
    std::string inputSchema() const override { return inSchema_; }
    std::string outputSchema() const override { return outSchema_; }
    int64_t priceLez() const override { return price_; }
    std::string execute(const std::string& args) override { return fn_(args); }

private:
    std::string name_, category_, desc_, inSchema_, outSchema_;
    int64_t price_;
    std::function<std::string(const std::string&)> fn_;
};

class SkillRegistry {
public:
    void registerSkill(std::unique_ptr<PilotSkill> skill);

    // Runtime third-party skill loader (Usability #1): scan `dir` for native skill
    // plugins and register the skills they provide, so an operator can add skills
    // WITHOUT recompiling the core module.
    //
    // TRUST / SAFETY (this is a native-code loader, not a sandbox):
    //   * OFF BY DEFAULT. Does nothing unless the operator sets PILOT_ENABLE_PLUGINS
    //     (any value other than empty/"0"/"false"). With it unset, no .so is opened
    //     and behavior is identical to a build without this loader.
    //   * `dir` is an OPERATOR-TRUSTED directory. A plugin loaded from it runs with
    //     the FULL privileges of the agent (keys, funds, DB, network). Placing a file
    //     there is an explicit act of trust — it is an operator boundary, NOT a
    //     security sandbox. No isolation is claimed or provided.
    //   * Per-plugin isolation is for ROBUSTNESS, not security: a plugin that fails to
    //     load, has the wrong IID, reports a mismatched ABI version, throws, or names a
    //     skill that already exists is logged and SKIPPED. A bad plugin never crashes
    //     the module, never double-registers a name, and is never silently treated as
    //     loaded.
    void loadPlugins(const std::string& dir);

    std::string listSkills() const;
    std::string listSkillsForCard() const;
    std::string dispatch(const std::string& name, const std::string& argsJson);
    bool hasSkill(const std::string& name) const;
    size_t count() const;

private:
    std::unordered_map<std::string, std::unique_ptr<PilotSkill>> skills_;
};

class PilotImpl;
void registerBuiltinSkills(SkillRegistry& registry, PilotImpl* impl);
