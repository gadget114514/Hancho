#pragma once

// Worker engine abstraction for Behavior Trees (doc/obsoleted/behavior-tree-plan.md section 4.3).
// Pure logic only: no GUI, no IPC, no process launching.

#include "nlohmann/json.hpp"

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
    // True when (engine, model) is approved and enabled in the named LLM list. The default
    // keeps catalogs that know nothing about approval lists (tests, plain registry) working.
    virtual bool IsApprovedForList(const std::string& /*list*/,
                                   const std::string& /*engine*/,
                                   const std::string& /*model*/) const {
        return true;
    }
};

struct WorkerEngineCapabilities {
    bool jsonStream = false;
    bool resume = false;
    bool reportsUsage = false;
    bool reportsSubagents = false;
    std::vector<std::string> permissionModes;
};

struct WorkerEngineDef {
    std::string command;
    std::string defaultModel;
    std::string stream;
    std::string permissionArgs;
    // 091-7: extra command arguments built from approved permits (claude --add-dir /
    // --allowedTools). Empty for engines that take permissions through the environment.
    std::string permitArgs;
    // 091-7: environment variable that receives the converted permission config. Empty
    // means the engine receives no engine-specific permit config (only HANCHO_WORKER_PERMITS).
    std::string permitEnv;
    // Permission flags used when the engine runs as the read-only triage advisor. Empty means
    // no bypass flags at all. Never use the worker permissionArgs here.
    std::string advicePermissionArgs;
    std::vector<std::string> models; // optional allowlist; empty accepts any valid model name
    std::string adapter;             // Tier 1/3 adapter name (doc/obsoleted/design-v3.md 1.5)
    WorkerEngineCapabilities capabilities;
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
    // resolves), {permissionArgs}, and {timeoutMin}. permissionArgsOverride replaces the engine's
    // worker permissionArgs (used to run the triage advisor read-only); nullptr keeps the engine's.
    bool BuildCommand(const std::string& engine,
                      const std::string& model,
                      const std::string& launchPrompt,
                      int timeoutSec,
                      std::string& outCommand,
                      std::string& outStreamType,
                      std::string& err,
                       std::string* outAdapter = nullptr,
                       const std::string* permissionArgsOverride = nullptr,
                       const std::string* permitArgsOverride = nullptr) const;

    // 091-7 conversion of approved permits for a specific engine. `approved` is an array of
    // permit rows (pc::permit::Entry::ToJson). Returns the JSON value for the engine's
    // permitEnv (empty when the engine has no engine-specific config) and appends a warning
    // for engines that cannot express the requested permits. HANCHO_WORKER_PERMITS is built
    // separately from the same array so arbitrary commands can read it too.
    std::string BuildPermitEnvironment(const std::string& engine, const nlohmann::json& approved,
                                       std::vector<std::string>& warnings) const;
    std::string BuildPermitArgs(const std::string& engine, const nlohmann::json& approved,
                                std::vector<std::string>& warnings) const;

private:
    std::string defaultEngine_;
    std::map<std::string, WorkerEngineDef> engines_;
};

// 091-7: the engine-neutral permit payload handed to every worker as HANCHO_WORKER_PERMITS.
std::string BuildWorkerPermitsJson(const nlohmann::json& approved);

// Validates a JSON string against an outputSchema (JSON Schema subset: "required" and "type",
// plus nested "properties" and "items"). Empty outputSchema always succeeds.
bool ValidateOutput(const std::string& jsonText, const std::string& outputSchemaJson, std::string& err);

} // namespace pc
