// Shared engine health registry (doc/HanchoExpectations.md 8.6).

#include "engine_health.h"
#include "worker_probe.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace pc {
namespace engine_health {

namespace {

std::string Timestamp(int64_t epochMs) {
    if (epochMs <= 0) return "";
    return GetCurrentTimestampStr();  // events use the process clock; deadlines use epoch ms
}

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool IsRateLimitedClass(const std::string& s) {
    const std::string l = ToLowerAscii(s);
    return l.find("rate") != std::string::npos || l.find("quota") != std::string::npos ||
           l.find("429") != std::string::npos || l.find("insufficient") != std::string::npos;
}

} // namespace

int64_t NowEpochMs() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string Key(const std::string& engine, const std::string& model) {
    return engine + "/" + model;
}

std::wstring DefaultPath() {
    return PathJoinWide(GetEffectiveDataDirectoryWide(), L"engine-health.json");
}

std::wstring EventsPath() {
    return PathJoinWide(GetEffectiveDataDirectoryWide(), L"engine-health.events.jsonl");
}

Settings LoadSettings() {
    Settings s;
    const std::string text = ReadFileUtf8Wide(PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json"));
    if (text.empty()) return s;
    json cfg = json::parse(text, nullptr, false);
    if (!cfg.is_object()) return s;
    auto eh = cfg.find("engine_health");
    if (eh == cfg.end() || !eh->is_object()) return s;
    auto getInt = [&](const char* key, int def) {
        auto it = eh->find(key);
        return (it != eh->end() && it->is_number()) ? it->get<int>() : def;
    };
    s.probeInitialSec = getInt("probe_initial_sec", kDefaultProbeInitialSec);
    if (s.probeInitialSec < 1) s.probeInitialSec = 1;
    s.probeMaxSec = getInt("probe_max_sec", kDefaultProbeMaxSec);
    if (s.probeMaxSec < s.probeInitialSec) s.probeMaxSec = s.probeInitialSec;
    s.recentSuccessSec = getInt("recent_success_sec", kDefaultRecentSuccessSec);
    if (s.recentSuccessSec < 1) s.recentSuccessSec = 1;
    s.probeTimeoutSec = getInt("probe_timeout_sec", 60);
    if (s.probeTimeoutSec < 1) s.probeTimeoutSec = 60;
    return s;
}

int64_t NextProbeDelayMs(const Settings& settings, int probeCount) {
    int64_t sec = settings.probeInitialSec;
    for (int i = 0; i < probeCount && sec < settings.probeMaxSec; ++i) {
        if (sec > settings.probeMaxSec / 2) { sec = settings.probeMaxSec; break; }
        sec *= 2;
    }
    if (sec > settings.probeMaxSec) sec = settings.probeMaxSec;
    return sec * 1000;
}

json Entry::ToJson() const {
    json j = {
        {"status", status},
        {"since", sinceMs},
        {"reason", reason},
        {"nextProbeAt", nextProbeAtMs},
        {"probeCount", probeCount},
        {"lastError", lastError},
        {"retryAt", retryAtMs},
        {"lastSuccessAt", lastSuccessMs},
        {"zeroEventFailures", zeroEventFailures},
    };
    return j;
}

Entry Entry::FromJson(const json& j, const std::string& engine, const std::string& model) {
    Entry e;
    e.engine = engine;
    e.model = model;
    if (!j.is_object()) return e;
    e.status = j.value("status", std::string("healthy"));
    if (e.status != "down") e.status = "healthy";
    e.sinceMs = j.value("since", static_cast<int64_t>(0));
    e.reason = j.value("reason", std::string());
    e.nextProbeAtMs = j.value("nextProbeAt", static_cast<int64_t>(0));
    e.probeCount = j.value("probeCount", 0);
    e.lastError = j.value("lastError", std::string());
    e.retryAtMs = j.value("retryAt", static_cast<int64_t>(0));
    e.lastSuccessMs = j.value("lastSuccessAt", static_cast<int64_t>(0));
    e.zeroEventFailures = j.value("zeroEventFailures", 0);
    return e;
}

std::string Entry::HoldReason() const {
    std::ostringstream oss;
    oss << "engine down: " << Key(engine, model) << " since " << Timestamp(sinceMs);
    if (!reason.empty()) oss << " (" << reason << ")";
    return oss.str();
}

json Store::ToJson() const {
    json engines = json::object();
    for (const auto& kv : entries) engines[kv.first] = kv.second.ToJson();
    return json{{"version", 1}, {"engines", engines}};
}

bool Load(const std::wstring& path, Store& out, std::string& err) {
    out = Store();
    if (!PathExistsWide(path)) return true;
    const std::string text = ReadFileUtf8Wide(path);
    if (text.empty()) return true;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "engine-health.json is not a JSON object";
        return false;
    }
    auto engines = j.find("engines");
    if (engines == j.end() || !engines->is_object()) return true;
    for (auto it = engines->begin(); it != engines->end(); ++it) {
        const std::string key = it.key();
        const size_t slash = key.find('/');
        const std::string engine = slash == std::string::npos ? key : key.substr(0, slash);
        const std::string model = slash == std::string::npos ? "" : key.substr(slash + 1);
        out.entries[key] = Entry::FromJson(it.value(), engine, model);
    }
    return true;
}

bool Save(const std::wstring& path, const Store& store, std::string& err) {
    try {
        WriteFileAtomicWide(path, store.ToJson().dump(2) + "\n");
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

Entry* Find(Store& store, const std::string& engine, const std::string& model) {
    auto it = store.entries.find(Key(engine, model));
    return it == store.entries.end() ? nullptr : &it->second;
}

const Entry* Find(const Store& store, const std::string& engine, const std::string& model) {
    auto it = store.entries.find(Key(engine, model));
    return it == store.entries.end() ? nullptr : &it->second;
}

bool IsDown(const Store& store, const std::string& engine, const std::string& model) {
    const Entry* e = Find(store, engine, model);
    return e && e->status == "down";
}

bool ProbeDue(const Entry& entry, int64_t nowMs) {
    if (entry.status != "down") return false;
    if (entry.nextProbeAtMs > nowMs) return false;
    if (entry.retryAtMs > nowMs) return false;
    return true;
}

bool IsDownWorthyFailure(const std::string& failureClass) {
    const std::string l = ToLowerAscii(failureClass);
    if (l.empty()) return false;
    return l == "startup_hang" || l == "probe_failed" || l == "startup_timed_out" ||
           l == "idle_stall" || IsRateLimitedClass(l) || l.find("quota") != std::string::npos;
}

void EmitEvent(const std::string& type, const std::string& engine, const std::string& model,
               const std::string& reason, int64_t sinceMs, int64_t nextProbeAtMs) {
    json ev = {
        {"type", type},
        {"engine", engine},
        {"model", model},
        {"key", Key(engine, model)},
        {"reason", reason},
        {"since", sinceMs},
        {"nextProbeAt", nextProbeAtMs},
        {"timestamp", GetCurrentTimestampStr()},
    };
    AppendToLogFileWide(EventsPath(), ev.dump() + "\r\n");
}

bool MarkDown(const std::wstring& path, const std::string& engine, const std::string& model,
              const std::string& failureClass, const std::string& detail, int64_t nowMs,
              int64_t retryAtMs, const Settings& settings) {
    if (!IsDownWorthyFailure(failureClass)) return false;
    Store store;
    std::string err;
    if (!Load(path, store, err)) return false;
    Entry& e = store.entries[Key(engine, model)];
    e.engine = engine;
    e.model = model;
    const bool wasDown = e.status == "down";
    if (!wasDown) {
        e.status = "down";
        e.sinceMs = nowMs;
        e.probeCount = 0;
        e.nextProbeAtMs = nowMs + NextProbeDelayMs(settings, 0);
    }
    e.reason = failureClass;
    e.lastError = detail.empty() ? failureClass : detail;
    if (retryAtMs > 0) e.retryAtMs = retryAtMs;
    if (e.retryAtMs > e.nextProbeAtMs) e.nextProbeAtMs = e.retryAtMs;
    if (!Save(path, store, err)) return false;
    if (!wasDown) EmitEvent("engineDown", engine, model, failureClass, e.sinceMs, e.nextProbeAtMs);
    return true;
}

bool MarkHealthy(const std::wstring& path, const std::string& engine, const std::string& model,
                 const std::string& reason, int64_t nowMs) {
    Store store;
    std::string err;
    if (!Load(path, store, err)) return false;
    Entry* e = Find(store, engine, model);
    if (!e) return true;
    const bool wasDown = e->status == "down";
    e->status = "healthy";
    e->sinceMs = 0;
    e->nextProbeAtMs = 0;
    e->retryAtMs = 0;
    e->probeCount = 0;
    e->zeroEventFailures = 0;
    e->lastSuccessMs = nowMs;
    e->reason.clear();
    e->lastError.clear();
    (void)reason;
    if (!Save(path, store, err)) return false;
    if (wasDown) EmitEvent("engineRecovered", engine, model, reason, 0, 0);
    return true;
}

bool MarkSuccess(const std::wstring& path, const std::string& engine, const std::string& model,
                 int64_t nowMs) {
    Store store;
    std::string err;
    if (!Load(path, store, err)) return false;
    Entry& e = store.entries[Key(engine, model)];
    e.engine = engine;
    e.model = model;
    e.lastSuccessMs = nowMs;
    e.zeroEventFailures = 0;
    return Save(path, store, err);
}

bool RecordZeroEventFailure(const std::wstring& path, const std::string& engine,
                            const std::string& model, int64_t nowMs) {
    Store store;
    std::string err;
    if (!Load(path, store, err)) return false;
    Entry& e = store.entries[Key(engine, model)];
    e.engine = engine;
    e.model = model;
    e.zeroEventFailures++;
    (void)nowMs;
    return Save(path, store, err);
}

bool RecordFailedProbe(const std::wstring& path, const std::string& engine,
                       const std::string& model, const std::string& failureClass,
                       const std::string& detail, int64_t nowMs, const Settings& settings) {
    Store store;
    std::string err;
    if (!Load(path, store, err)) return false;
    Entry* e = Find(store, engine, model);
    if (!e || e->status != "down") return true;
    e->probeCount++;
    e->reason = failureClass.empty() ? "probe_failed" : failureClass;
    e->lastError = detail.empty() ? e->reason : detail;
    e->nextProbeAtMs = nowMs + NextProbeDelayMs(settings, e->probeCount);
    if (e->retryAtMs > e->nextProbeAtMs) e->nextProbeAtMs = e->retryAtMs;
    return Save(path, store, err);
}

bool Reset(const std::wstring& path, const std::string& engine, const std::string& model,
           int64_t nowMs) {
    return MarkHealthy(path, engine, model, "manual reset", nowMs);
}

ProbeRunResult RunDueHealthProbes(const std::wstring& healthPath, int64_t nowMs,
                                  const Settings& settings) {
    ProbeRunResult result;
    Store store;
    std::string err;
    if (!Load(healthPath, store, err)) return result;
    std::vector<std::pair<std::string, std::string>> due;
    for (const auto& kv : store.entries) {
        if (ProbeDue(kv.second, nowMs)) due.push_back({kv.second.engine, kv.second.model});
    }
    if (due.empty()) return result;

    WorkerEngineRegistry registry;
    const std::wstring enginesPath = PathJoinWide(GetEffectiveDataDirectoryWide(), L"engines.json");
    if (PathExistsWide(enginesPath)) registry.LoadFromJson(ReadFileUtf8Wide(enginesPath), err);
    ProbeOptions options;
    options.timeoutSec = settings.probeTimeoutSec;
    options.noCache = true;   // health probes must not be answered from the 10-minute cache
    options.ttlSuccessSec = 0;
    options.ttlFailureSec = 0;
    for (const auto& em : due) {
        ProbeOutcome out = ProbeEngine(registry, em.first, em.second, options);
        result.probed++;
        if (out.ok) {
            if (MarkHealthy(healthPath, em.first, em.second, "probe succeeded", NowEpochMs()))
                result.recovered++;
        } else {
            const std::string cls = out.failureClass.empty() ? "probe_failed" : out.failureClass;
            if (RecordFailedProbe(healthPath, em.first, em.second, cls, out.detail, NowEpochMs(), settings))
                result.failed++;
        }
    }
    return result;
}

} // namespace engine_health
} // namespace pc
