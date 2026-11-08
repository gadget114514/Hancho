#pragma once

#include "../src/behavior_tree.h"
#include "../src/bt_debug_view.h"
#include "../src/common.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace bt_test {

using namespace pc;

extern int g_failures;
extern int g_passes;

inline void Check(bool cond, const std::string& label) {
    std::printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", label.c_str());
    std::fflush(stdout);
    if (cond) g_passes++;
    else g_failures++;
}

class FakeEngineCatalog : public IBtEngineCatalog {
public:
    std::string DefaultEngine() const override { return "agy"; }

    bool Knows(const std::string& engine, const std::string& model, std::string& err) const override {
        std::string eng = engine.empty() ? DefaultEngine() : engine;
        if (eng != "agy" && eng != "claude" && eng != "opencode" && eng != "codex") {
            err = "unknown engine: " + eng;
            return false;
        }
        if (model == "bad-model") {
            err = "unknown model: " + model;
            return false;
        }
        return true;
    }
};

class FakeJobRunner : public IBtJobRunner {
public:
    bool failNextLaunch = false;
    std::vector<BtJobSpec> startedJobs;
    std::vector<BtWorkerSpec> startedWorkers;
    std::vector<std::string> cancelledJobs;

    std::string StartJob(const BtJobSpec& spec) override {
        if (failNextLaunch) {
            failNextLaunch = false;
            return "";
        }
        startedJobs.push_back(spec);
        return spec.proposedJobId;
    }

    std::string StartWorker(const BtWorkerSpec& spec) override {
        if (failNextLaunch) {
            failNextLaunch = false;
            return "";
        }
        startedWorkers.push_back(spec);
        return spec.proposedJobId;
    }

    void CancelJob(const std::string& jobId) override {
        cancelledJobs.push_back(jobId);
    }

    bool WasCancelled(const std::string& jobId) const {
        return std::find(cancelledJobs.begin(), cancelledJobs.end(), jobId) != cancelledJobs.end();
    }
};

void RunPart2Tests(const std::string& tempDirUtf8, const std::wstring& tempDirW);

} // namespace bt_test
