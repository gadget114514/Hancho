#pragma once

#include "common.h"
#include "llm_stream.h"
#include "TerminalView.h"
#include <mutex>
#include <thread>
#include <memory>
#include <atomic>
#include <functional>

namespace pc {

struct StartProcessRequest {
    std::string name;
    std::string commandLine;
    std::string workingDir;
    std::string logFile;     // optional custom log file path
    std::string doneFile;    // optional custom completion marker file path
    ExecMode mode = ExecMode::Pipe;
    bool openStdin = true;   // pipe mode: keep stdin writable; false closes it after launch so children see EOF
    int timeoutSec = 0;      // 0 = no limit
    int idleTimeoutSec = 0;  // 0 = no limit (output idle)
    int idleCpuSec = 0;      // if > 0, require CPU idle too (AND with output idle)
    std::string estimatedEndTimeStr;  // optional estimated end time for display
    std::map<std::string, std::string> env;  // extra environment variables (UTF-8)
    std::vector<std::string> killExclude;    // image names excluded from tree kill (reserved)
    std::string jobKey;      // taskqueue job id (display / callback correlation)
    int jobId = 0;           // taskqueue numeric job id
    int attemptN = 0;        // taskqueue attempt number
    std::string agentType;   // "claude", "agy", "opencode", "auto", or ""
    bool jsonStream = false; // enable real-time JSONL stream parsing & logging
    std::string supervisorNotes; // context/instructions provided by supervisor LLM
    std::string supervisorGoal;  // high-level objective provided by supervisor LLM
};

struct ProcessSnapshot {
    int id = 0;
    std::string name;
    std::string commandLine;
    std::string workingDir;
    ExecMode mode = ExecMode::Pipe;
    ProcState state = ProcState::Running;
    DWORD rootPid = 0;
    DWORD exitCode = STILL_ACTIVE;
    std::string startTimeStr;
    std::string endTimeStr;
    int64_t startEpochSec = 0;
    int64_t endEpochSec = 0;
    std::string estimatedEndTimeStr;
    uint64_t startTickMs = 0;
    uint64_t endTickMs = 0;
    uint64_t lastOutputTickMs = 0;
    uint64_t elapsedMs = 0;
    uint64_t idleMs = 0;
    uint64_t cpuTimeMs = 0;
    uint64_t lastCpuActivityTickMs = 0;
    int timeoutSec = 0;
    int idleTimeoutSec = 0;
    uint32_t treeProcessCount = 0;
    uint64_t treeMemoryBytes = 0;
    uint64_t totalBytesRead = 0;
    uint64_t outputVersion = 0;
    std::string lastLinePreview;
    std::string logFilePath;
    std::string doneFilePath;
    HWND terminalHwnd = nullptr;
    std::string jobKey;
    int jobId = 0;
    int attemptN = 0;
    std::string agentType;
    bool jsonStream = false;
    std::string supervisorNotes;
    std::string supervisorGoal;
    std::string agentStatus;
    std::string currentTool;
    std::string currentToolAction;
    std::string lastThought;
    std::string lastMessage;
    std::string activity;          // LlmAgentState::activity
    std::string activityTarget;
    int64_t activitySinceEpochMs = 0;
    int stepCount = 0;
    int streamEventCount = 0;
    std::string streamFilePath;
};

class ProcessManager;

struct AgentExtractedResult {
    bool found = false;
    std::string result;
    std::string source;
    bool isError = false;
    bool truncated = false;
    ProcessSnapshot snap;
    LlmAgentState agentState;
};

// Shared helper to extract a worker's final response from its LLM stream (or fallback output),
// used by IPC run_worker/wait_worker and by Behavior Tree worker nodes (${<id>.result}).
AgentExtractedResult ExtractAgentResult(ProcessManager& pm, int id, bool finished, int settleWaitMs = 100);

class ProcessManager {
public:
    using CreateTerminalCallback = std::function<TerminalView*(
        const std::wstring& resolvedCmd,
        const std::wstring& cwd,
        std::function<void(const char*, size_t)> onOutput,
        std::wstring& outError)>;
    using DestroyTerminalCallback = std::function<void(TerminalView*)>;
    // Invoked on the monitor thread when a managed process reaches a terminal
    // state. endKind is one of: completed, failed, timed_out, idle_timed_out, killed.
    using ProcessFinishedCallback = std::function<void(int processId, int exitCode, const std::string& endKind)>;

    ProcessManager();
    ~ProcessManager();

    void SetProcessFinishedCallback(ProcessFinishedCallback cb) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        finishedCb_ = std::move(cb);
    }

    void SetTerminalCallbacks(CreateTerminalCallback createCb, DestroyTerminalCallback destroyCb) {
        createTerminalCb_ = std::move(createCb);
        destroyTerminalCb_ = std::move(destroyCb);
    }

    // Start a new managed process. Returns assigned process ID (>0) or -1 on error.
    int StartProcess(const StartProcessRequest& req, std::string& outError);

    // Send input string to stdin / ConPTY of process
    bool SendInput(int id, const std::string& text, bool appendNewline, std::string& outError);

    // Terminate process tree via Job Object
    bool KillProcess(int id, std::string& outError);

    // Terminate all running processes
    int KillAllRunning();

    // Remove finished processes from list
    int ClearFinished();
    bool RemoveProcess(int id);

    // Renumber all processes sequentially from #1 (requires no running processes).
    // Returns the number of renumbered processes, or -1 on error.
    int ResetProcessIds(std::string& outError);

    // Bring separate console window to foreground
    bool FocusProcessWindow(int id);

    // Get TerminalView* for a process ID (returns nullptr if not a ConPTY process)
    TerminalView* GetTerminalView(int id);

    // Get snapshot of a single process
    bool GetSnapshot(int id, ProcessSnapshot& outSnap);

    // Get snapshots of all managed processes
    std::vector<ProcessSnapshot> GetAllSnapshots();

    // Get process output log (reads from TerminalBuffer for ConPTY processes)
    bool GetOutput(int id, bool stripAnsi, int tailLines, std::string& outText, uint64_t* outVersion = nullptr);

    // LLM stream inspection and supervisor context
    bool GetAgentStream(int id, int tail, const std::string& typeFilter, uint64_t sinceSeq,
                        std::vector<LlmStreamEvent>& outEvents, LlmAgentState& outState);
    bool UpdateAgentInfo(int id, const std::string& notes, const std::string& goal);

    // Wait until a process finishes or maxWaitMs elapses (returns true if finished)
    bool WaitForExit(int id, uint32_t maxWaitMs);

private:
    struct ManagedProcess {
        int id = 0;
        std::string name;
        std::string commandLine;
        std::string workingDir;
        ExecMode mode = ExecMode::Pipe;
        std::atomic<ProcState> state{ProcState::Running};

        DWORD rootPid = 0;
        std::atomic<DWORD> exitCode{STILL_ACTIVE};

        HANDLE hProcess = nullptr;
        HANDLE hThread = nullptr;
        HANDLE hJob = nullptr;
        HANDLE hStdinWrite = nullptr;
        HANDLE hStdoutRead = nullptr;

        // Pseudo-PTY Terminal (src/Terminal + include)
        TerminalView* terminalView = nullptr;
        DestroyTerminalCallback destroyTerminalCb;

        // Headless ConPTY fallback when GUI callbacks are not registered
        std::unique_ptr<ConPtySession> headlessPty;
        std::unique_ptr<TerminalBuffer> headlessBuffer;
        std::unique_ptr<TerminalEmulator> headlessEmulator;

        std::string startTimeStr;
        std::string endTimeStr;
        int64_t startEpochSec = 0;
        int64_t endEpochSec = 0;
        std::string estimatedEndTimeStr;
        uint64_t startTickMs = 0;
        uint64_t restoredElapsedMs = 0;
        std::atomic<uint64_t> endTickMs{0};
        std::atomic<uint64_t> lastOutputTickMs{0};
        std::atomic<uint64_t> cpuTime100ns{0};
        std::atomic<uint64_t> lastCpuActivityTickMs{0};
        int timeoutSec = 0;
        int idleTimeoutSec = 0;
        int idleCpuSec = 0;
        std::string jobKey;
        int jobId = 0;
        int attemptN = 0;

        std::atomic<uint32_t> treeProcessCount{0};
        std::atomic<uint64_t> treeMemoryBytes{0};

        std::mutex outputMutex;
        std::string rawOutput;
        std::string cleanOutput;
        std::string lastLinePreview;
        std::string logFilePath;
        std::wstring logFilePathWide;
        std::string doneFilePath;
        std::wstring doneFilePathWide;
        std::wstring defaultDoneFilePathWide;
        uint64_t totalBytesRead = 0;
        uint64_t outputVersion = 0;

        std::thread readerThread;

        // LLM JSON output streaming tracker
        std::unique_ptr<LlmStreamTracker> streamTracker;
        std::string agentType;
        std::string supervisorNotes;
        std::string supervisorGoal;
        bool jsonStream = false;

        ~ManagedProcess();
    };

    void ReaderThreadFunc(std::shared_ptr<ManagedProcess> proc);
    void MonitorThreadFunc();
    void UpdateJobMetrics(ManagedProcess& proc);
    void TerminateJobInternal(ManagedProcess& proc, ProcState finalState, const std::string& reasonMsg);
    void NotifyProcessFinished(ManagedProcess& proc, int exitCode, const std::string& endKind);
    static void WriteDoneFile(ManagedProcess& proc, ProcState finalState, DWORD exitCode, const std::string& endTimeStr, uint64_t elapsedMs, const std::string& reasonMsg);
    static ProcessSnapshot BuildSnapshot(ManagedProcess& p, uint64_t now);
    void SaveState();
    void LoadState();

    std::mutex mutex_;
    std::vector<std::shared_ptr<ManagedProcess>> processes_;
    int nextId_ = 1;
    std::atomic<bool> stopMonitor_{false};
    std::thread monitorThread_;
    CreateTerminalCallback createTerminalCb_;
    DestroyTerminalCallback destroyTerminalCb_;
    ProcessFinishedCallback finishedCb_;
    std::mutex cbMutex_;
};

} // namespace pc
