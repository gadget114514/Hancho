#pragma once

// Worker engine abstraction for Behavior Trees (doc/behavior-tree-plan.md section 4.3).
// Pure logic only: no GUI, no IPC, no process launching.

#include <map>
#include <string>
#include <vector>

namespace pc {

// Quote a string as a single Windows command-line argument (same rule as TaskQueue {summary}).
std::string QuoteCommandArg(const std::string& s);

// Minimal interface queried by the BT core at validation time so it never depends on concrete CLIs.
class IBtEngineCatalog {
public:
    virtual ~IBtEngineCatalog() = default;
    virtual std::string DefaultEngine() const = 0;
    virtual bool Knows(const std::string& engine, const std::string& model, std::string& err) const = 0;
};

struct WorkerEngineDef {
    std::string command;
    std::string defaultModel;
    std::string stream;
    std::string permissionArgs;
    std::vector<std::string> models; // optional allowlist; empty accepts any valid model name
};

class WorkerEngineRegistry : public IBtEngineCatalog {
public:
    WorkerEngineRegistry();
    ~WorkerEngineRegistry() override = default;

    void ResetToBuiltins();

    // Loads and overrides/adds engine definitions from an engines.json string (section 4.3).
    bool LoadFromJson(const std::string& jsonText, std::string& err);

    std::string DefaultEngine() const override { return defaultEngine_; }
    void SetDefaultEngine(const std::string& engine) { defaultEngine_ = engine; }

    const WorkerEngineDef* Find(const std::string& engine) const;
    const std::map<std::string, WorkerEngineDef>& Engines() const { return engines_; }

    bool Knows(const std::string& engine, const std::string& model, std::string& err) const override;

    // Fills {prompt} (quoted as one argument), {model} (dropping "--model {model}" when no model
    // resolves), {permissionArgs}, and {timeoutMin}.
    bool BuildCommand(const std::string& engine,
                      const std::string& model,
                      const std::string& launchPrompt,
                      int timeoutSec,
                      std::string& outCommand,
                      std::string& outStreamType,
                      std::string& err) const;

private:
    std::string defaultEngine_;
    std::map<std::string, WorkerEngineDef> engines_;
};

// Validates a JSON string against an outputSchema (JSON Schema subset: "required" and "type",
// plus nested "properties" and "items"). Empty outputSchema always succeeds.
bool ValidateOutput(const std::string& jsonText, const std::string& outputSchemaJson, std::string& err);

} // namespace pc
