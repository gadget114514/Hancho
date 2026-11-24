#pragma once

// Shared engine health / circuit-breaker state (doc/obsoleted/HanchoExpectations.md 8.6).
// Pure logic plus file persistence: no GUI, no IPC and no process launching.
// Callers pass a path so the state machine is trivially testable.

#include "common.h"

#include "nlohmann/json.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pc {
namespace engine_health {

using nlohmann::json;

constexpr int kDefaultProbeInitialSec = 300;
constexpr int kDefaultProbeMaxSec = 3600;
constexpr int kDefaultRecentSuccessSec = 21600;

struct Settings {
    int probeInitialSec = kDefaultProbeInitialSec;
    int probeMaxSec = kDefaultProbeMaxSec;
    int recentSuccessSec = kDefaultRecentSuccessSec;
    int probeTimeoutSec = 60;
};

struct Entry {
    std::string engine;
    std::string model;
    std::string status = "healthy";   // "healthy" | "down"
    int64_t sinceMs = 0;
    std::string reason;               // failureClass that tripped the breaker
    int64_t nextProbeAtMs = 0;
    int probeCount = 0;
    std::string lastError;
    int64_t retryAtMs = 0;            // explicit quota reset time: never probe before this
    int64_t lastSuccessMs = 0;
    int zeroEventFailures = 0;        // recent failures with no worker activity events

    json ToJson() const;
    static Entry FromJson(const json& j, const std::string& engine, const std::string& model);
    std::string HoldReason() const;
};

struct Store {
    std::map<std::string, Entry> entries;   // key: "engine/model"

    json ToJson() const;
};

std::string Key(const std::string& engine, const std::string& model);
std::wstring DefaultPath();
std::wstring EventsPath();

int64_t NowEpochMs();
Settings LoadSettings();
int64_t NextProbeDelayMs(const Settings& settings, int probeCount);

bool Load(const std::wstring& path, Store& out, std::string& err);
bool Save(const std::wstring& path, const Store& store, std::string& err);

Entry* Find(Store& store, const std::string& engine, const std::string& model);
const Entry* Find(const Store& store, const std::string& engine, const std::string& model);
bool IsDown(const Store& store, const std::string& engine, const std::string& model);

bool ProbeDue(const Entry& entry, int64_t nowMs);
bool IsDownWorthyFailure(const std::string& failureClass);

bool MarkDown(const std::wstring& path, const std::string& engine, const std::string& model,
              const std::string& failureClass, const std::string& detail, int64_t nowMs,
              int64_t retryAtMs, const Settings& settings);
bool MarkHealthy(const std::wstring& path, const std::string& engine, const std::string& model,
                 const std::string& reason, int64_t nowMs);
bool MarkSuccess(const std::wstring& path, const std::string& engine, const std::string& model,
                 int64_t nowMs);
bool RecordZeroEventFailure(const std::wstring& path, const std::string& engine,
                            const std::string& model, int64_t nowMs);
bool RecordFailedProbe(const std::wstring& path, const std::string& engine,
                       const std::string& model, const std::string& failureClass,
                       const std::string& detail, int64_t nowMs, const Settings& settings);
bool Reset(const std::wstring& path, const std::string& engine, const std::string& model,
           int64_t nowMs);
void EmitEvent(const std::string& type, const std::string& engine, const std::string& model,
               const std::string& reason, int64_t sinceMs, int64_t nextProbeAtMs);

struct ProbeRunResult {
    int probed = 0;
    int recovered = 0;
    int failed = 0;
};
// Called by the core scheduler: probe every down engine whose nextProbeAt (and retryAt) passed.
ProbeRunResult RunDueHealthProbes(const std::wstring& healthPath, int64_t nowMs,
                                  const Settings& settings);

} // namespace engine_health
} // namespace pc
