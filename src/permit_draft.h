#pragma once

#include "nlohmann/json.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace pc {

class BtManager;
class ProcessManager;

// 091-7 follow-up: asks the worker LLM to draft one permit row from a plain-language wish.
//
// The draft runs asynchronously, like the triage advisor: Start() launches a read-only engine
// process and returns a draft id at once; Get() reports "running", "done" (with the proposed
// fields) or "failed". Nothing is saved to permits.json here. The dashboard fills the form and
// the person still presses Request, so the LLM only proposes.
class PermitDraftManager {
public:
    PermitDraftManager(ProcessManager& pm, BtManager* btm);

    // Returns {"ok":true,"draftId":...} or {"ok":false,"error":...}.
    std::string Start(const std::string& requestText);
    // Returns {"ok":true,"state":"running"|"done"|"failed","fields":{...},"error":...}.
    std::string Get(const std::string& draftId);

private:
    struct Draft {
        std::wstring resultPath;
        std::wstring logPath;
        std::chrono::steady_clock::time_point started;
    };

    ProcessManager& pm_;
    BtManager* btm_ = nullptr;
    std::mutex mutex_;
    std::map<std::string, Draft> drafts_;
    unsigned long long nextId_ = 1;
};

} // namespace pc
