#include "permit_draft.h"

#include "bt_manager.h"
#include "common.h"
#include "permit_config.h"
#include "process_manager.h"
#include "worker_engines.h"

#include <algorithm>
#include <vector>

namespace pc {
namespace {

using json = nlohmann::json;

// The launched process gets this long to answer; Get() gives up a little after it.
constexpr int kDraftTimeoutSec = 300;
constexpr auto kDraftGiveUp = std::chrono::seconds(kDraftTimeoutSec + 30);
// Only the tail of a log can hold the final answer; keeps the scan cheap on long logs.
constexpr size_t kLogTailBytes = 256 * 1024;

std::string TrimDraftText(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

json Error(const std::string& message) {
    return json{{"ok", false}, {"error", message}};
}

std::string StrField(const json& obj, const char* key) {
    auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// Accepts one permit object and normalizes it to the dashboard's text fields.
bool ParsePermitFields(const json& obj, json& out) {
    if (!obj.is_object()) return false;
    const std::string kind = TrimDraftText(StrField(obj, "kind"));
    if (!permit::IsValidKind(kind)) return false;
    const std::string target = TrimDraftText(StrField(obj, "target"));
    if (target.empty()) return false;

    std::string engines;
    auto it = obj.find("engines");
    if (it != obj.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_string()) continue;
            const std::string name = TrimDraftText(item.get<std::string>());
            if (name.empty()) continue;
            if (!engines.empty()) engines += ",";
            engines += name;
        }
    } else if (it != obj.end() && it->is_string()) {
        engines = TrimDraftText(it->get<std::string>());
    }

    out = json{{"kind", kind},
               {"target", target},
               {"engines", engines},
               {"scope", TrimDraftText(StrField(obj, "scope"))},
               {"note", TrimDraftText(StrField(obj, "note"))}};
    return true;
}

// Finds the last JSON object in text that is a valid permit. The engine's final message can
// arrive inside a JSON-escaped log line, so the text is also tried with escaped quotes undone.
bool ExtractPermitFields(const std::string& text, json& out) {
    const std::string unescaped = [&] {
        std::string s;
        s.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '"') {
                s += '"';
                ++i;
            } else {
                s += text[i];
            }
        }
        return s;
    }();

    bool found = false;
    for (const std::string* source : { &text, &unescaped }) {
        const std::string& s = *source;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] != '{') {
                ++i;
                continue;
            }
            // Find the matching close brace, skipping braces inside JSON strings.
            int depth = 0;
            bool inString = false;
            size_t end = std::string::npos;
            for (size_t j = i; j < s.size(); ++j) {
                const char c = s[j];
                if (inString) {
                    if (c == '\\') ++j;
                    else if (c == '"') inString = false;
                    continue;
                }
                if (c == '"') inString = true;
                else if (c == '{') ++depth;
                else if (c == '}' && --depth == 0) {
                    end = j;
                    break;
                }
            }
            if (end == std::string::npos) break;
            json candidate = json::parse(s.substr(i, end - i + 1), nullptr, false);
            json fields;
            if (!candidate.is_discarded() && ParsePermitFields(candidate, fields)) {
                out = fields;
                found = true;
                i = end + 1;
            } else {
                ++i;
            }
        }
    }
    return found;
}

std::string ReadLogTail(const std::wstring& path) {
    std::string text = ReadFileUtf8Wide(path);
    if (text.size() > kLogTailBytes) text = text.substr(text.size() - kLogTailBytes);
    return text;
}

} // namespace

PermitDraftManager::PermitDraftManager(ProcessManager& pm, BtManager* btm) : pm_(pm), btm_(btm) {}

std::string PermitDraftManager::Start(const std::string& requestText) {
    const std::string wish = TrimDraftText(requestText);
    if (wish.empty()) return Error("request is empty").dump();
    if (!btm_) return Error("no worker engine is configured").dump();

    const WorkerEngineRegistry& engines = btm_->Engines();
    const std::string engineName = engines.DefaultEngine();
    const WorkerEngineDef* engineDef = engineName.empty() ? nullptr : engines.Find(engineName);
    if (!engineDef) return Error("no worker engine is configured").dump();

    std::string draftId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        draftId = "pd-" + std::to_string(nextId_++) + "-" +
                  std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count());
    }

    const std::wstring baseDir = PathJoinWide(GetEffectiveDataDirectoryWide(), L"permit_drafts");
    EnsureDirectoryWide(baseDir);
    const std::wstring safeId = Utf8ToWide(SanitizeFileNameComponent(draftId));
    const std::wstring instructionPath = PathJoinWide(baseDir, safeId + L".md");
    const std::wstring resultPath = PathJoinWide(baseDir, safeId + L".result.json");
    const std::wstring logPath = PathJoinWide(baseDir, safeId + L".log");
    RemoveFileWide(resultPath);

    // The engine only proposes. It is told not to act, and the dashboard still needs a person to
    // press Request, so nothing here writes permits.json.
    const std::string instruction =
        "# Permit draft\n\n"
        "The person wants this, in their own words:\n\n"
        "<<<\n" + wish + "\n>>>\n\n"
        "Hancho permits let a worker do one thing. Propose ONE permit row that covers the wish, "
        "and keep it as narrow as possible.\n\n"
        "Fields:\n"
        "- kind: one of read_path, write_path, command, network.\n"
        "  - read_path: a file or folder the worker may read.\n"
        "  - write_path: a file or folder the worker may write.\n"
        "  - command: a command line the worker may run.\n"
        "  - network: a host the worker may connect to.\n"
        "- target: the path, command or host. The wildcards * and ? are allowed.\n"
        "- engines: array of engine names allowed to use it; [] means all engines.\n"
        "- scope: \"\" for global, otherwise a scope such as taskqueue:<name> or bt:<id>.\n"
        "- note: one short sentence explaining why.\n\n"
        "Do not run commands, do not write files, and do not approve anything. "
        "Reply with exactly one JSON object as your final message and nothing else, in this form:\n"
        "{\"kind\": \"...\", \"target\": \"...\", \"engines\": [], \"scope\": \"\", \"note\": \"...\"}\n";
    WriteFileAtomicWide(instructionPath, instruction);

    const std::string launchPrompt = "Read the instructions file " + WideToUtf8(instructionPath) +
        ". This is a read-only drafting task: do not run commands or ask for approval. Reply with the "
        "required JSON object as your final message.";
    std::string command, streamType, commandError, adapter;
    if (!engines.BuildCommand(engineName, std::string(), launchPrompt, kDraftTimeoutSec, command,
                              streamType, commandError, &adapter, &engineDef->advicePermissionArgs)) {
        return Error(commandError.empty() ? "failed to build the engine command" : commandError).dump();
    }

    StartProcessRequest req;
    req.name = "permit-draft";
    req.commandLine = command;
    req.workingDir = WideToUtf8(GetEffectiveDataDirectoryWide());
    req.mode = ExecMode::Pipe;
    req.openStdin = false;
    req.timeoutSec = kDraftTimeoutSec;
    req.logFile = WideToUtf8(logPath);
    req.agentType = engineName;
    req.jsonStream = true;
    req.supervisorGoal = "Permit draft";
    req.supervisorNotes = "Draft a permit row from the person's wish";
    req.env["PC_PERMIT_RESULT_FILE"] = WideToUtf8(resultPath);
    if (!adapter.empty()) {
        req.agentType = adapter;
        req.jsonStream = true;
    }
    std::string startError;
    const int processId = pm_.StartProcess(req, startError);
    if (processId <= 0) {
        return Error(startError.empty() ? "failed to start the drafting engine" : startError).dump();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    drafts_[draftId] = Draft{resultPath, logPath, std::chrono::steady_clock::now()};
    return json{{"ok", true}, {"draftId", draftId}}.dump();
}

std::string PermitDraftManager::Get(const std::string& draftId) {
    Draft draft;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = drafts_.find(draftId);
        if (it == drafts_.end()) return Error("unknown draft id").dump();
        draft = it->second;
    }

    json fields;
    // The result file is written when the engine can write files; otherwise the final message
    // is only in the log.
    if (ParsePermitFields(json::parse(ReadFileUtf8Wide(draft.resultPath), nullptr, false), fields) ||
        ExtractPermitFields(ReadLogTail(draft.logPath), fields)) {
        return json{{"ok", true}, {"state", "done"}, {"fields", fields}}.dump();
    }

    const auto elapsed = std::chrono::steady_clock::now() - draft.started;
    if (elapsed > kDraftGiveUp) {
        return json{{"ok", true}, {"state", "failed"},
                    {"error", "the engine did not return a usable permit draft"}}.dump();
    }
    return json{{"ok", true}, {"state", "running"}}.dump();
}

} // namespace pc
