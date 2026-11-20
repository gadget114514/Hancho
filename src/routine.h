#pragma once

#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace pc {

class RoutineManager {
public:
    using Validator = std::function<bool(const std::string& btJson, std::string& outError)>;

    void SetValidator(Validator v) { validator_ = std::move(v); }

    std::string Save(const std::string& name,
                     const std::string& btJson,
                     const std::vector<std::string>& inputs,
                     const std::string& summary,
                     const std::string& notes,
                     const std::string& scope,
                     const std::string& cwd,
                     const std::string& by,
                     const std::string& reason);

    // 093-5: a definition created by a worker LLM is stored as a proposal next to the
    // approved one. The approved file (<dir>/<name>.json) stays untouched until approval.
    std::string SaveProposal(const std::string& name,
                             const std::string& btJson,
                             const std::vector<std::string>& inputs,
                             const std::string& summary,
                             const std::string& notes,
                             const std::string& scope,
                             const std::string& cwd,
                             const std::string& by,
                             const std::string& reason,
                             const nlohmann::json& proposalMeta);
    // Accepts a proposal. `via` is "chat" (userSaid is the user's words; empty rejects) or
    // "dashboard" (button click). On success the proposed definition becomes the approved one.
    std::string Approve(const std::string& name, const std::string& cwd,
                        const std::string& userSaid, const std::string& by,
                        const std::string& via);
    std::string Reject(const std::string& name, const std::string& cwd,
                       const std::string& reason, const std::string& by);
    // Registers a previously successful BT (<data-dir>/behaviortrees/<btId>.json) as a routine.
    std::string SaveFromBt(const std::string& name, const std::string& btId,
                           const std::string& scope, const std::string& cwd,
                           const std::string& by, bool proposed,
                           const std::string& reason);

    std::string List(const std::string& cwd) const;
    std::string Show(const std::string& name, const std::string& cwd) const;
    std::string Check(const std::string& name, const std::string& cwd) const;
    std::string PrepareRun(const std::string& name, const std::string& cwd) const;

    static std::wstring ProposalDirWide(const std::string& scope, const std::string& cwd);
    static std::wstring ProposalPathWide(const std::string& name, const std::string& scope,
                                         const std::string& cwd, std::string* outScope = nullptr,
                                         bool* outFound = nullptr);
    static nlohmann::json ReadProposal(const std::string& name, const std::string& cwd,
                                       std::string* outScope = nullptr, bool* outFound = nullptr);
    static bool HasProposal(const std::string& name, const std::string& cwd);

    bool AppendRun(const std::string& name, const nlohmann::json& run, std::string& outError) const;
    bool UpdateLatestRun(const std::string& name, const nlohmann::json& patch, std::string& outError) const;
    // Re-capture the inputs snapshot after a successful run of a stale routine.
    bool RefreshInputsSnapshot(const std::string& name, const std::string& cwd,
                               const std::string& by, std::string& outError) const;

    static bool ValidName(const std::string& name);
    static std::string DataDirUtf8();
    static std::wstring RoutineDirWide(const std::string& scope, const std::string& cwd);
    static std::wstring DefinitionPathWide(const std::string& name, const std::string& scope,
                                           const std::string& cwd, std::string* outScope = nullptr,
                                           bool* outFound = nullptr);
    static std::wstring RunsPathWide(const std::string& name);
    static nlohmann::json ReadRuns(const std::string& name);
    static std::string MakeRunId(const std::string& name, const std::string& runsJson);

private:
    Validator validator_;
};

} // namespace pc
