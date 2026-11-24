#pragma once

// Health probe (doc/obsoleted/HanchoExpectations.md 8.3). Uses the engine registry's launch
// command with a fixed minimal prompt, closes stdin, and waits for the first event
// plus a clean exit. Results are cached per engine+model in <data-dir>/probes.json.

#include "llm_config.h"
#include "worker_engines.h"

#include <string>

namespace pc {

struct ProbeOutcome {
    bool ok = false;
    int elapsedMs = 0;
    std::string failureClass;  // "", "startup_hang", "auth_error", or "probe_failed"
    std::string detail;
    bool cached = false;
    std::string engine;
    std::string model;

    nlohmann::json ToJson() const;
};

struct ProbeOptions {
    int timeoutSec = 60;
    bool noCache = false;
    int ttlSuccessSec = 600;
    int ttlFailureSec = 120;
};

std::wstring ProbeCacheDefaultPath();

ProbeOutcome ProbeEngine(const WorkerEngineRegistry& registry,
                         const std::string& engine,
                         const std::string& model,
                         const ProbeOptions& options,
                         const std::wstring& cachePath);

inline ProbeOutcome ProbeEngine(const WorkerEngineRegistry& registry,
                                const std::string& engine,
                                const std::string& model,
                                const ProbeOptions& options) {
    return ProbeEngine(registry, engine, model, options, ProbeCacheDefaultPath());
}

} // namespace pc
