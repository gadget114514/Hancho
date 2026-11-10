#include "process_manager.h"
#include "nlohmann/json.hpp"
#include <sstream>
#include <algorithm>

namespace pc {

static std::wstring ResolveCommandLineForWin32(const std::string& rawCmdUtf8) {
    std::wstring cmd = Utf8ToWide(rawCmdUtf8);
    size_t start = cmd.find_first_not_of(L" \t");
    if (start == std::wstring::npos) return L"";
    cmd = cmd.substr(start);

    std::wstring prog;
    if (cmd[0] == L'"') {
        size_t endQuote = cmd.find(L'"', 1);
        if (endQuote != std::wstring::npos) {
            prog = cmd.substr(1, endQuote - 1);
        } else {
            prog = cmd.substr(1);
        }
    } else {
        size_t space = cmd.find_first_of(L" \t");
        prog = (space == std::wstring::npos) ? cmd : cmd.substr(0, space);
    }

    auto endsWithCI = [](const std::wstring& s, const std::wstring& suffix) {
        if (s.size() < suffix.size()) return false;
        return _wcsicmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
    };

    auto getCmdExePath = []() -> std::wstring {
        wchar_t path[MAX_PATH] = {};
        ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\cmd.exe", path, MAX_PATH);
        return path;
    };

    if (endsWithCI(prog, L".cmd") || endsWithCI(prog, L".bat")) {
        return getCmdExePath() + L" /d /s /c \"" + cmd + L"\"";
    }

    if (!endsWithCI(prog, L".exe")) {
        wchar_t foundBuf[MAX_PATH] = {};
        if (SearchPathW(nullptr, prog.c_str(), L".exe", MAX_PATH, foundBuf, nullptr) == 0) {
            if (SearchPathW(nullptr, prog.c_str(), L".cmd", MAX_PATH, foundBuf, nullptr) > 0 ||
                SearchPathW(nullptr, prog.c_str(), L".bat", MAX_PATH, foundBuf, nullptr) > 0) {
                return getCmdExePath() + L" /d /s /c \"" + cmd + L"\"";
            }
        }
    }

    return cmd;
}

static std::vector<wchar_t> BuildChildEnvironment(const std::wstring& cwd, const std::map<std::string, std::string>& extra) {    std::vector<wchar_t> block;
    auto append = [&block](const std::wstring& entry) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    };

    auto isOverridden = [&extra](const std::wstring& key) {
        std::string k = WideToUtf8(key);
        for (const auto& kv : extra) {
            if (_stricmp(kv.first.c_str(), k.c_str()) == 0) return true;
        }
        return false;
    };

    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (LPWCH p = env; *p; ) {
            std::wstring entry(p);
            p += entry.size() + 1;
            if (entry[0] != L'=') {
                size_t eq = entry.find(L'=');
                std::wstring key = (eq == std::wstring::npos) ? entry : entry.substr(0, eq);
                if (_wcsicmp(key.c_str(), L"PWD") == 0 ||
                    _wcsicmp(key.c_str(), L"OLDPWD") == 0 ||
                    _wcsicmp(key.c_str(), L"INIT_CWD") == 0) {
                    continue;
                }
                if (isOverridden(key)) {
                    continue;
                }
            }
            append(entry);
        }
        FreeEnvironmentStringsW(env);
    }

    if (!cwd.empty()) {
        append(L"PWD=" + cwd);
        append(L"INIT_CWD=" + cwd);
    }

    for (const auto& kv : extra) {
        append(Utf8ToWide(kv.first) + L"=" + Utf8ToWide(kv.second));
    }

    block.push_back(L'\0');
    return block;
}

static std::wstring MakeEventPipeName() {
    static std::atomic<uint64_t> counter{0};
    return L"\\\\.\\pipe\\HanchoEvent_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring(GetNowTickMs()) + L"_" + std::to_wstring(counter.fetch_add(1));
}

static HANDLE CreateEventPipeServer(const std::wstring& name) {
    return CreateNamedPipeW(name.c_str(),
                            PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            1, 64 * 1024, 64 * 1024, 0, nullptr);
}

ProcessManager::ManagedProcess::~ManagedProcess() {
    if (hStdinWrite) {
        CloseHandle(hStdinWrite);
        hStdinWrite = nullptr;
    }
    if (hStdoutRead) {
        CloseHandle(hStdoutRead);
        hStdoutRead = nullptr;
    }
    eventPipeStop = true;
    if (eventPipeThread.joinable()) {
        CancelSynchronousIo(eventPipeThread.native_handle());
        eventPipeThread.join();
    }
    eventTracker.store(nullptr);
    if (hEventPipe) {
        CloseHandle(hEventPipe);
        hEventPipe = nullptr;
    }
    if (readerThread.joinable()) {
        readerThread.join();
    }
    if (terminalView) {
        if (destroyTerminalCb) {
            destroyTerminalCb(terminalView);
        } else {
            delete terminalView;
        }
        terminalView = nullptr;
        hProcess = nullptr;
        hThread = nullptr;
    }
    if (headlessPty) {
        headlessPty->Close();
        headlessPty.reset();
        hProcess = nullptr;
        hThread = nullptr;
    }
    if (hThread) {
        CloseHandle(hThread);
        hThread = nullptr;
    }
    if (tree) {
        // Closing a job with KILL_ON_JOB_CLOSE terminates the tree; keep that
        // behaviour by killing explicitly before releasing the boundary object.
        tree->Kill(1);
        tree.reset();
    }
    if (hProcess) {
        CloseHandle(hProcess);
        hProcess = nullptr;
    }
}

ProcessManager::ProcessManager() {
    LoadState();
    // If the monitor loop throws, log it and restart it: exit detection and timeouts must keep working.
    monitorThread_ = std::thread([this] {
        while (!stopMonitor_) {
            try {
                MonitorThreadFunc();
                return;
            } catch (const std::exception& e) {
                WriteAuditLog("CRASH_GUARD", std::string("Monitor thread exception: ") + e.what());
            } catch (...) {
                WriteAuditLog("CRASH_GUARD", "Monitor thread exception: unknown");
            }
            Sleep(200);
        }
    });
}

ProcessManager::~ProcessManager() {
    stopMonitor_ = true;
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }
    KillAllRunning();
    SaveState();
    std::lock_guard<std::mutex> lock(mutex_);
    processes_.clear();
}

int ProcessManager::StartProcess(const StartProcessRequest& req, std::string& outError) {
    if (req.commandLine.empty()) {
        outError = "Command line cannot be empty.";
        return -1;
    }

    std::wstring resolvedCmd = ResolveCommandLineForWin32(req.commandLine);
    if (resolvedCmd.empty()) {
        outError = "Failed to resolve command line.";
        return -1;
    }

    std::wstring cwdWide = ResolveAbsolutePathWide(Utf8ToWide(req.workingDir));
    const wchar_t* cwdPtr = cwdWide.c_str();

    // HANCHO_EVENT_PIPE: a per-process named pipe that the CLI (or a wrapper) can write
    // hancho.event/1 lines to. Only pipe-mode workers with streaming get one.
    std::map<std::string, std::string> childEnvVars = req.env;
    std::wstring eventPipeName;
    HANDLE hEventPipe = nullptr;
    if (req.mode == ExecMode::Pipe && (req.jsonStream || !req.agentType.empty()) &&
        req.env.find("HANCHO_EVENT_PIPE") == req.env.end()) {
        eventPipeName = MakeEventPipeName();
        hEventPipe = CreateEventPipeServer(eventPipeName);
        if (hEventPipe) {
            childEnvVars["HANCHO_EVENT_PIPE"] = WideToUtf8(eventPipeName);
        } else {
            eventPipeName.clear();
        }
    }
    std::vector<wchar_t> childEnv = BuildChildEnvironment(cwdWide, childEnvVars);

    auto proc = std::make_shared<ManagedProcess>();
    proc->tree = platform::CreateProcessTree();
    if (!proc->tree) {
        if (hEventPipe) CloseHandle(hEventPipe);
        outError = "Failed to create the process tree boundary.";
        return -1;
    }
    proc->hEventPipe = hEventPipe;
    proc->eventPipeName = eventPipeName;
    if (proc->hEventPipe) {
        proc->eventPipeThread = std::thread([proc] { EventPipeThreadFunc(proc); });
    }
    proc->commandLine = req.commandLine;
    proc->workingDir = WideToUtf8(cwdWide);
    proc->mode = req.mode;
    proc->timeoutSec = req.timeoutSec;
    proc->idleTimeoutSec = req.idleTimeoutSec;
    proc->idleCpuSec = req.idleCpuSec;
    proc->jobKey = req.jobKey;
    proc->jobId = req.jobId;
    proc->attemptN = req.attemptN;
    proc->estimatedEndTimeStr = req.estimatedEndTimeStr;
    proc->destroyTerminalCb = destroyTerminalCb_;

    uint64_t now = GetNowTickMs();
     proc->startTimeStr = GetCurrentTimestampStr();
     proc->startEpochSec = static_cast<int64_t>(time(nullptr));
    proc->startTickMs = now;
    proc->lastOutputTickMs = now;
    proc->lastCpuActivityTickMs = now;

    if (req.mode == ExecMode::ConPTY) {
        std::weak_ptr<ManagedProcess> weakProc = proc;
        auto outputObserver = [weakProc](const char* data, size_t len) {
            auto sp = weakProc.lock();
            if (!sp || len == 0) return;
            sp->lastOutputTickMs = GetNowTickMs();
            std::string chunk(data, len);
            std::string cleanChunk = StripAnsiCodes(chunk);
            std::wstring logPathW;
            {
                std::lock_guard<std::mutex> outLock(sp->outputMutex);
                sp->rawOutput.append(chunk);
                sp->cleanOutput.append(cleanChunk);
                sp->totalBytesRead += len;
                sp->outputVersion++;
                logPathW = sp->logFilePathWide;
                constexpr size_t kMaxRawBytes = 2 * 1024 * 1024;
                if (sp->rawOutput.size() > kMaxRawBytes) {
                    sp->rawOutput.erase(0, sp->rawOutput.size() - kMaxRawBytes);
                }
                if (sp->cleanOutput.size() > kMaxRawBytes) {
                    sp->cleanOutput.erase(0, sp->cleanOutput.size() - kMaxRawBytes);
                }
            }
            if (!logPathW.empty() && !cleanChunk.empty()) {
                AppendToLogFileWide(logPathW, cleanChunk);
            }
            if (sp->streamTracker && !cleanChunk.empty()) {
                sp->streamTracker->IngestChunk(cleanChunk.data(), cleanChunk.size());
            }
        };

        if (createTerminalCb_) {
            std::wstring werr;
            TerminalView* tv = createTerminalCb_(resolvedCmd, cwdWide, outputObserver, werr);
            if (!tv) {
                outError = "TerminalView ConPTY start failed: " + WideToUtf8(werr);
                return -1;
            }
            proc->terminalView = tv;
            proc->hProcess = tv->GetProcessHandle();
            proc->rootPid = tv->ProcessId();
        } else {
            // Headless ConPtySession + TerminalEmulator + TerminalBuffer fallback
            proc->headlessBuffer = std::make_unique<TerminalBuffer>(120, 40);
            proc->headlessEmulator = std::make_unique<TerminalEmulator>();
            proc->headlessEmulator->reset(proc->headlessBuffer.get());
            proc->headlessPty = std::make_unique<ConPtySession>();
            proc->headlessPty->SetWorkingDirectory(cwdWide);
            proc->headlessEmulator->setResponseCallback([weakProc](const std::wstring& resp) {
                if (auto sp = weakProc.lock()) {
                    if (sp->headlessPty) sp->headlessPty->WriteFront(resp);
                }
            });
            proc->headlessPty->SetOutputCallback([weakProc, outputObserver](const char* data, size_t len) {
                outputObserver(data, len);
                auto sp = weakProc.lock();
                if (!sp) return;
                std::wstring ws = Utf8ToWide(std::string(data, len));
                std::lock_guard<std::mutex> outLock(sp->outputMutex);
                if (sp->headlessEmulator) {
                    sp->headlessEmulator->process(ws);
                }
            });
            if (!proc->headlessPty->Start(resolvedCmd, 120, 40)) {
                outError = "ConPtySession::Start failed: " + WideToUtf8(proc->headlessPty->LastError());
                return -1;
            }
            proc->hProcess = proc->headlessPty->GetProcessHandle();
            proc->rootPid = proc->headlessPty->ProcessId();
        }

        if (proc->hProcess && proc->tree) {
            std::string attachError;
            proc->tree->Attach(proc->rootPid, attachError);
        }
        proc->treeProcessCount = 1;
    } else {
        PROCESS_INFORMATION pi = {};
        BOOL created = FALSE;
        DWORD lastErr = 0;

        std::vector<wchar_t> cmdMutable(resolvedCmd.begin(), resolvedCmd.end());
        cmdMutable.push_back(L'\0');

        if (req.mode == ExecMode::Pipe) {
            SECURITY_ATTRIBUTES sa = {};
            sa.nLength = sizeof(SECURITY_ATTRIBUTES);
            sa.bInheritHandle = TRUE;

            HANDLE hChildStdinRead = nullptr;
            HANDLE hChildStdoutWrite = nullptr;

            if (!CreatePipe(&hChildStdinRead, &proc->hStdinWrite, &sa, 0) ||
                !CreatePipe(&proc->hStdoutRead, &hChildStdoutWrite, &sa, 0)) {
                outError = "Failed to create anonymous pipes.";
                return -1;
            }

            SetHandleInformation(proc->hStdinWrite, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(proc->hStdoutRead, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOW si = {};
            si.cb = sizeof(STARTUPINFOW);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdInput = hChildStdinRead;
            si.hStdOutput = hChildStdoutWrite;
            si.hStdError = hChildStdoutWrite;

            created = CreateProcessW(
                nullptr,
                cmdMutable.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                childEnv.data(),
                cwdPtr,
                &si,
                &pi
            );
            lastErr = GetLastError();

            CloseHandle(hChildStdinRead);
            CloseHandle(hChildStdoutWrite);

        } else { // ExecMode::Console
            STARTUPINFOW si = {};
            si.cb = sizeof(STARTUPINFOW);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOW;

            created = CreateProcessW(
                nullptr,
                cmdMutable.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NEW_CONSOLE | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                childEnv.data(),
                cwdPtr,
                &si,
                &pi
            );
            lastErr = GetLastError();
        }

        if (!created) {
            outError = "CreateProcessW failed (Win32 error " + std::to_string(lastErr) + ") for command: " + req.commandLine;
            return -1;
        }

        if (req.mode == ExecMode::Pipe && !req.openStdin && proc->hStdinWrite) {
            CloseHandle(proc->hStdinWrite);
            proc->hStdinWrite = nullptr;
        }

        if (proc->tree) {
            std::string attachError;
            proc->tree->Attach(pi.dwProcessId, attachError);
        }
        ResumeThread(pi.hThread);

        proc->hProcess = pi.hProcess;
        proc->hThread = pi.hThread;
        proc->rootPid = pi.dwProcessId;
        proc->treeProcessCount = 1;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        proc->id = nextId_++;
        if (req.name.empty()) {
            proc->name = "proc-" + std::to_string(proc->id);
        } else {
            proc->name = req.name;
        }

        std::string safeName = SanitizeFileNameComponent(proc->name);
        std::string fileTs = GetFileTimestampStr();
        std::wstring logsDirW = GetLogsDirectoryWide();

        if (!req.logFile.empty()) {
            proc->logFilePathWide = Utf8ToWide(req.logFile);
            proc->logFilePath = req.logFile;
        } else {
            proc->logFilePathWide = logsDirW + L"\\proc_" + std::to_wstring(proc->id) + L"_" + Utf8ToWide(safeName) + L"_" + Utf8ToWide(fileTs) + L".log";
            proc->logFilePath = WideToUtf8(proc->logFilePathWide);
        }

        proc->defaultDoneFilePathWide = logsDirW + L"\\proc_" + std::to_wstring(proc->id) + L".done";
        if (!req.doneFile.empty()) {
            proc->doneFilePathWide = Utf8ToWide(req.doneFile);
            proc->doneFilePath = req.doneFile;
        } else {
            proc->doneFilePathWide = proc->defaultDoneFilePathWide;
            proc->doneFilePath = WideToUtf8(proc->doneFilePathWide);
        }

        // Ensure any stale completion marker files do NOT exist while the process is running
        DeleteFileW(proc->defaultDoneFilePathWide.c_str());
        if (proc->doneFilePathWide != proc->defaultDoneFilePathWide) {
            DeleteFileW(proc->doneFilePathWide.c_str());
        }

        std::string detectedAgent = req.agentType;
        if (detectedAgent.empty() || detectedAgent == "auto") {
            detectedAgent = DetectAgentType(proc->commandLine, proc->name);
        }
        proc->agentType = detectedAgent;
        proc->jsonStream = req.jsonStream || (!detectedAgent.empty());
        proc->supervisorNotes = req.supervisorNotes;
        proc->supervisorGoal = req.supervisorGoal;

        if (proc->jsonStream) {
            std::wstring streamPathW = logsDirW + L"\\proc_" + std::to_wstring(proc->id) + L"_" + Utf8ToWide(safeName) + L"_stream.jsonl";
            proc->streamTracker = std::make_unique<LlmStreamTracker>();
            std::string defaultModel;
            {
                const std::string& cl = proc->commandLine;
                size_t pos = std::string::npos;
                size_t optLen = 0;
                pos = cl.find("--model");
                if (pos != std::string::npos) {
                    optLen = 7;
                } else {
                    size_t p = 0;
                    while (true) {
                        size_t found = cl.find("-m", p);
                        if (found == std::string::npos) break;
                        bool boundaryBefore = (found == 0) || std::isspace(static_cast<unsigned char>(cl[found - 1])) ||
                                             cl[found - 1] == '"' || cl[found - 1] == '\'';
                        size_t afterOpt = found + 2;
                        bool boundaryAfter = (afterOpt >= cl.size()) || std::isspace(static_cast<unsigned char>(cl[afterOpt])) ||
                                            cl[afterOpt] == '=' || cl[afterOpt] == '"' || cl[afterOpt] == '\'';
                        if (boundaryBefore && boundaryAfter) {
                            pos = found;
                            optLen = 2;
                            break;
                        }
                        p = found + 2;
                    }
                }
                if (pos != std::string::npos) {
                    size_t after = pos + optLen;
                    while (after < cl.size() && (cl[after] == ' ' || cl[after] == '=' || cl[after] == '"' || cl[after] == '\'')) after++;
                    size_t end = after;
                    while (end < cl.size() && cl[end] != ' ' && cl[end] != '"' && cl[end] != '\'') end++;
                    if (end > after) defaultModel = cl.substr(after, end - after);
                }
            }
            proc->streamTracker->Init(proc->id, proc->agentType, WideToUtf8(streamPathW), proc->supervisorNotes,
                                      proc->supervisorGoal, defaultModel,
                                      proc->jobId > 0 ? std::to_string(proc->jobId) : std::string(),
                                      proc->attemptN);
            proc->eventTracker.store(proc->streamTracker.get());
        }

        {
            std::ostringstream hdr;
            hdr << "================================================================================\r\n"
                << "Hancho Process Log\r\n"
                << "ID          : #" << proc->id << "\r\n"
                << "Name        : " << proc->name << "\r\n"
                << "Command     : " << proc->commandLine << "\r\n"
                << "Working Dir : " << (proc->workingDir.empty() ? "(default)" : proc->workingDir) << "\r\n"
                << "Mode        : " << ExecModeToString(proc->mode) << "\r\n"
                << "Root PID    : " << proc->rootPid << "\r\n"
                << "Start Time  : " << proc->startTimeStr << "\r\n"
                << "Log File    : " << proc->logFilePath << "\r\n"
                << "Done File   : " << proc->doneFilePath << "\r\n"
                << "================================================================================\r\n\r\n";

            std::string earlyClean;
            {
                std::lock_guard<std::mutex> outLock(proc->outputMutex);
                earlyClean = proc->cleanOutput;
            }
            AppendToLogFileWide(proc->logFilePathWide, hdr.str() + earlyClean);
        }

        WriteAuditLog("PROCESS_START",
                      "Started #" + std::to_string(proc->id) + " [" + proc->name + "] PID=" + std::to_string(proc->rootPid) +
                      " Mode=" + ExecModeToString(proc->mode) + " Cmd=\"" + proc->commandLine +
                      "\" Log=\"" + proc->logFilePath + "\" DoneFile=\"" + proc->doneFilePath + "\"");

        if (proc->mode == ExecMode::Console) {
            std::lock_guard<std::mutex> outLock(proc->outputMutex);
            std::string msg = "[Hancho] Started in separate Console Window (PID " + std::to_string(proc->rootPid) + ")\n"
                              "Command: " + proc->commandLine + "\n";
            proc->rawOutput = msg;
            proc->cleanOutput = msg;
            proc->lastLinePreview = "Running in separate Console window (PID " + std::to_string(proc->rootPid) + ")";
            proc->outputVersion++;
            AppendToLogFileWide(proc->logFilePathWide, msg);
        } else if (proc->mode == ExecMode::Pipe && proc->hStdoutRead) {
            proc->readerThread = std::thread([this, proc] {
                try {
                    ReaderThreadFunc(proc);
                } catch (const std::exception& e) {
                    WriteAuditLog("CRASH_GUARD", "Reader thread exception (#" + std::to_string(proc->id) + "): " + e.what());
                } catch (...) {
                    WriteAuditLog("CRASH_GUARD", "Reader thread exception (#" + std::to_string(proc->id) + "): unknown");
                }
            });
        }

        processes_.push_back(proc);
    }
    SaveState();
    return proc->id;
}

void ProcessManager::EventPipeThreadFunc(std::shared_ptr<ManagedProcess> proc) {
    if (!proc || !proc->hEventPipe) return;
    std::string buffer;
    std::vector<std::string> backlog;

    auto flushBacklog = [&](LlmStreamTracker* tracker) {
        if (!tracker || backlog.empty()) return;
        for (const auto& line : backlog) tracker->IngestExternalEventLine(line);
        backlog.clear();
    };

    while (!proc->eventPipeStop.load()) {
        LlmStreamTracker* tracker = proc->eventTracker.load();
        flushBacklog(tracker);

        if (!proc->eventPipeConnected) {
            BOOL ok = ConnectNamedPipe(proc->hEventPipe, nullptr);
            DWORD err = ok ? ERROR_SUCCESS : GetLastError();
            if (ok || err == ERROR_PIPE_CONNECTED) {
                proc->eventPipeConnected = true;
            } else {
                if (proc->eventPipeStop.load()) break;
                Sleep(20);
                continue;
            }
        }

        char chunk[4096];
        DWORD bytesRead = 0;
        if (!ReadFile(proc->hEventPipe, chunk, sizeof(chunk), &bytesRead, nullptr) || bytesRead == 0) {
            DisconnectNamedPipe(proc->hEventPipe);
            proc->eventPipeConnected = false;
            if (proc->eventPipeStop.load()) break;
            continue;
        }
        buffer.append(chunk, bytesRead);
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            tracker = proc->eventTracker.load();
            if (tracker) {
                flushBacklog(tracker);
                tracker->IngestExternalEventLine(line);
            } else if (backlog.size() < 1000) {
                backlog.push_back(std::move(line));
            }
        }
        constexpr size_t kMaxEventPipeBuffer = 1024 * 1024;
        if (buffer.size() > kMaxEventPipeBuffer) buffer.clear();
    }
}

void ProcessManager::ReaderThreadFunc(std::shared_ptr<ManagedProcess> proc) {
    char buffer[4096];
    DWORD bytesRead = 0;
    while (proc->hStdoutRead) {
        BOOL ok = ReadFile(proc->hStdoutRead, buffer, sizeof(buffer), &bytesRead, nullptr);
        if (!ok || bytesRead == 0) {
            break;
        }
        uint64_t now = GetNowTickMs();
        proc->lastOutputTickMs = now;

        std::string chunk(buffer, bytesRead);
        std::string cleanChunk = StripAnsiCodes(chunk);
        std::wstring logPathW;

        {
            std::lock_guard<std::mutex> outLock(proc->outputMutex);
            proc->rawOutput.append(chunk);
            proc->cleanOutput.append(cleanChunk);
            proc->totalBytesRead += bytesRead;
            proc->outputVersion++;
            logPathW = proc->logFilePathWide;

            constexpr size_t kMaxLogBytes = 2 * 1024 * 1024;
            if (proc->cleanOutput.size() > kMaxLogBytes) {
                proc->cleanOutput.erase(0, proc->cleanOutput.size() - kMaxLogBytes);
            }
            if (proc->rawOutput.size() > kMaxLogBytes) {
                proc->rawOutput.erase(0, proc->rawOutput.size() - kMaxLogBytes);
            }

            std::string preview = GetLastNonEmptyLine(proc->cleanOutput);
            if (!preview.empty()) {
                proc->lastLinePreview = preview;
            }
        }
        if (!logPathW.empty() && !cleanChunk.empty()) {
            AppendToLogFileWide(logPathW, cleanChunk);
        }
        if (proc->streamTracker && !cleanChunk.empty()) {
            proc->streamTracker->IngestChunk(cleanChunk.data(), cleanChunk.size());
        }
    }
}

void ProcessManager::UpdateJobMetrics(ManagedProcess& proc) {
    if (!proc.tree) return;

    platform::ProcessMetrics metrics;
    if (!proc.tree->Query(metrics)) return;

    uint64_t total100ns = metrics.cpuTimeMs * 10000ull;
    uint64_t prev = proc.cpuTime100ns.exchange(total100ns);
    if (total100ns > prev) {
        proc.lastCpuActivityTickMs = GetNowTickMs();
    }
    proc.treeProcessCount = metrics.processCount;
    proc.treeMemoryBytes = metrics.memoryBytes;
}

void ProcessManager::WriteDoneFile(ManagedProcess& proc, ProcState finalState, DWORD exitCode,
                                   const std::string& endTimeStr, uint64_t elapsedMs, const std::string& reasonMsg) {
    std::ostringstream oss;
    oss << "{\r\n"
        << "  \"id\": " << proc.id << ",\r\n"
        << "  \"name\": \"" << JsonEscape(proc.name) << "\",\r\n"
        << "  \"command\": \"" << JsonEscape(proc.commandLine) << "\",\r\n"
        << "  \"state\": \"" << ProcStateToString(finalState) << "\",\r\n"
        << "  \"exit_code\": " << exitCode << ",\r\n"
        << "  \"pid\": " << proc.rootPid << ",\r\n"
        << "  \"start_time\": \"" << JsonEscape(proc.startTimeStr) << "\",\r\n"
        << "  \"end_time\": \"" << JsonEscape(endTimeStr) << "\",\r\n"
        << "  \"estimated_end_time\": \"" << JsonEscape(proc.estimatedEndTimeStr) << "\",\r\n"
        << "  \"elapsed_ms\": " << elapsedMs << ",\r\n"
        << "  \"cpu_time_ms\": " << (proc.cpuTime100ns.load() / 10000) << ",\r\n"
        << "  \"uptime_str\": \"" << JsonEscape(FormatUptimeHMS(elapsedMs)) << "\",\r\n"
        << "  \"reason\": \"" << JsonEscape(reasonMsg) << "\",\r\n"
        << "  \"log_file\": \"" << JsonEscape(proc.logFilePath) << "\",\r\n"
        << "  \"done_file\": \"" << JsonEscape(proc.doneFilePath) << "\"\r\n"
        << "}\r\n";

    std::string payload = oss.str();
    if (!proc.defaultDoneFilePathWide.empty()) {
        WriteFileAtomicWide(proc.defaultDoneFilePathWide, payload);
    }
    if (!proc.doneFilePathWide.empty() && proc.doneFilePathWide != proc.defaultDoneFilePathWide) {
        WriteFileAtomicWide(proc.doneFilePathWide, payload);
    }
}

void ProcessManager::TerminateJobInternal(ManagedProcess& proc, ProcState finalState, const std::string& reasonMsg) {
    const bool treeKilled = proc.tree && proc.tree->Kill(1);
    if (!treeKilled && proc.hProcess) {
        TerminateProcess(proc.hProcess, 1);
    }

    if (proc.hStdinWrite) {
        CloseHandle(proc.hStdinWrite);
        proc.hStdinWrite = nullptr;
    }

    uint64_t endNow = GetNowTickMs();
    proc.endTickMs = endNow;
    proc.exitCode = 1;
    proc.state = finalState;
    proc.treeProcessCount = 0;
    proc.treeMemoryBytes = 0;

    uint64_t elapsedMs = (endNow >= proc.startTickMs) ? (endNow - proc.startTickMs) : 0;
    std::string endTimeStr = GetCurrentTimestampStr();
     proc.endTimeStr = endTimeStr;
     proc.endEpochSec = static_cast<int64_t>(time(nullptr));
    proc.restoredElapsedMs = elapsedMs;

    if (!reasonMsg.empty()) {
        std::lock_guard<std::mutex> outLock(proc.outputMutex);
        std::string line = "\n[Hancho] " + reasonMsg + "\n";
        proc.rawOutput += line;
        proc.cleanOutput += line;
        proc.lastLinePreview = reasonMsg;
        proc.outputVersion++;
    }

    if (!proc.logFilePathWide.empty()) {
        std::ostringstream ftr;
        ftr << "\r\n================================================================================\r\n"
            << "[Hancho] Process Terminated (" << ProcStateToString(finalState) << ")\r\n"
            << "Reason      : " << reasonMsg << "\r\n"
            << "End Time    : " << endTimeStr << "\r\n"
            << "Uptime      : " << FormatUptimeHMS(elapsedMs) << " (" << FormatDurationSec(elapsedMs) << ")\r\n"
            << "Done File   : " << proc.doneFilePath << "\r\n";
        if (proc.mode == ExecMode::ConPTY && proc.terminalView) {
            std::string screenSnap = WideToUtf8(proc.terminalView->ExtractScreenText(0));
            if (!screenSnap.empty()) {
                ftr << "------------------------ Final Terminal Screen Snapshot ------------------------\r\n"
                    << screenSnap
                    << "--------------------------------------------------------------------------------\r\n";
            }
        }
        ftr << "================================================================================\r\n";
        AppendToLogFileWide(proc.logFilePathWide, ftr.str());
    }

    WriteDoneFile(proc, finalState, 1, endTimeStr, elapsedMs, reasonMsg);

    WriteAuditLog("PROCESS_TERMINATED",
                  "Process #" + std::to_string(proc.id) + " [" + proc.name + "] State=" + ProcStateToString(finalState) +
                  " Uptime=" + FormatUptimeHMS(elapsedMs) + " Reason=\"" + reasonMsg + "\" DoneFile=\"" + proc.doneFilePath + "\"");

    NotifyProcessFinished(proc, 1, ProcStateToString(finalState));
}

void ProcessManager::NotifyProcessFinished(ManagedProcess& proc, int exitCode, const std::string& endKind) {
    ProcessFinishedCallback cb;
    {
        std::lock_guard<std::mutex> lock(cbMutex_);
        cb = finishedCb_;
    }
    if (cb) {
        cb(proc.id, exitCode, endKind);
    }
}

void ProcessManager::MonitorThreadFunc() {
    while (!stopMonitor_) {
        std::vector<std::shared_ptr<ManagedProcess>> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            copy = processes_;
        }

        uint64_t now = GetNowTickMs();
        for (auto& proc : copy) {
            if (proc->state != ProcState::Running) continue;

            if (proc->hProcess) {
                DWORD waitRes = WaitForSingleObject(proc->hProcess, 0);
                if (waitRes == WAIT_OBJECT_0) {
                    DWORD code = 0;
                    GetExitCodeProcess(proc->hProcess, &code);
                    // The root exited, but it may have left descendants (python, cmd, ...) running.
                    // Kill whatever is still in the job so nothing is orphaned.
                    if (proc->tree) {
                        proc->tree->Kill(0);
                    }
                    proc->exitCode = code;
                    proc->endTickMs = now;
                    ProcState finalSt = (code == 0) ? ProcState::Completed : ProcState::Failed;
                    proc->state = finalSt;

                    if (proc->hStdinWrite) {
                        CloseHandle(proc->hStdinWrite);
                        proc->hStdinWrite = nullptr;
                    }
                    proc->treeProcessCount = 0;
                    proc->treeMemoryBytes = 0;

                    if (proc->mode == ExecMode::Console) {
                        std::lock_guard<std::mutex> outLock(proc->outputMutex);
                        std::string msg = "[Hancho] Console process exited with code " + std::to_string(code) + "\n";
                        proc->cleanOutput += msg;
                        proc->rawOutput += msg;
                        proc->lastLinePreview = "Exited (code " + std::to_string(code) + ")";
                        proc->outputVersion++;
                    }

                    uint64_t elapsedMs = (now >= proc->startTickMs) ? (now - proc->startTickMs) : 0;
                    std::string endTimeStr = GetCurrentTimestampStr();
                    proc->endTimeStr = endTimeStr;
     proc->endEpochSec = static_cast<int64_t>(time(nullptr));
                    proc->restoredElapsedMs = elapsedMs;
                    if (!proc->logFilePathWide.empty()) {
                        std::ostringstream ftr;
                        ftr << "\r\n================================================================================\r\n"
                            << "[Hancho] Process Exited (" << ProcStateToString(finalSt) << ")\r\n"
                            << "Exit Code   : " << code << "\r\n"
                            << "End Time    : " << endTimeStr << "\r\n"
                            << "Uptime      : " << FormatUptimeHMS(elapsedMs) << " (" << FormatDurationSec(elapsedMs) << ")\r\n"
                            << "Done File   : " << proc->doneFilePath << "\r\n";
                        if (proc->mode == ExecMode::ConPTY && proc->terminalView) {
                            std::string screenSnap = WideToUtf8(proc->terminalView->ExtractScreenText(0));
                            if (!screenSnap.empty()) {
                                ftr << "------------------------ Final Terminal Screen Snapshot ------------------------\r\n"
                                    << screenSnap
                                    << "--------------------------------------------------------------------------------\r\n";
                            }
                        }
                        ftr << "================================================================================\r\n";
                        AppendToLogFileWide(proc->logFilePathWide, ftr.str());
                    }

                    WriteDoneFile(*proc, finalSt, code, endTimeStr, elapsedMs, "Exited normally");

                    WriteAuditLog("PROCESS_EXIT",
                                  "Process #" + std::to_string(proc->id) + " [" + proc->name + "] exited with code " +
                                  std::to_string(code) + " Uptime=" + FormatUptimeHMS(elapsedMs) +
                                  " DoneFile=\"" + proc->doneFilePath + "\"");
                    NotifyProcessFinished(*proc, static_cast<int>(code), ProcStateToString(finalSt));
                    SaveState();
                    continue;
                }
            }

            UpdateJobMetrics(*proc);

            if (proc->timeoutSec > 0) {
                uint64_t elapsedSec = (now - proc->startTickMs) / 1000;
                if (elapsedSec >= static_cast<uint64_t>(proc->timeoutSec)) {
                    TerminateJobInternal(*proc, ProcState::TimedOut,
                        "Process tree terminated: Exceeded runtime timeout (" + std::to_string(proc->timeoutSec) + "s)");
                    SaveState();
                    continue;
                }
            }

            if (proc->idleTimeoutSec > 0) {
                uint64_t lastOutput = proc->lastOutputTickMs.load();
                uint64_t lastCpu = proc->lastCpuActivityTickMs.load();
                uint64_t outputIdleSec = (now >= lastOutput) ? (now - lastOutput) / 1000 : 0;
                uint64_t cpuIdleSec = (now >= lastCpu) ? (now - lastCpu) / 1000 : 0;
                bool hung = false;
                if (proc->idleCpuSec > 0) {
                    hung = outputIdleSec >= static_cast<uint64_t>(proc->idleTimeoutSec) &&
                           cpuIdleSec >= static_cast<uint64_t>(proc->idleCpuSec);
                } else {
                    uint64_t lastActivity = (std::max)(lastOutput, lastCpu);
                    hung = (now >= lastActivity) &&
                           ((now - lastActivity) / 1000 >= static_cast<uint64_t>(proc->idleTimeoutSec));
                }
                if (hung) {
                    TerminateJobInternal(*proc, ProcState::IdleTimedOut,
                        "Process tree terminated: Exceeded idle/hang timeout (" + std::to_string(proc->idleTimeoutSec) + "s without output or CPU activity)");
                    SaveState();
                    continue;
                }
            }
        }

        Sleep(200);
    }
}

bool ProcessManager::SendInput(int id, const std::string& text, bool appendNewline, std::string& outError) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) {
        outError = "Process ID " + std::to_string(id) + " not found.";
        return false;
    }
    if (proc->state != ProcState::Running) {
        outError = "Process ID " + std::to_string(id) + " is not running.";
        return false;
    }

    if (proc->mode == ExecMode::ConPTY) {
        std::string payload = text;
        if (appendNewline) payload += "\r\n";
        bool sent = false;
        if (proc->terminalView) {
            proc->terminalView->SendInput(payload);
            sent = true;
        } else if (proc->headlessPty) {
            proc->headlessPty->Write(payload);
            sent = true;
        }
        if (sent) {
            proc->lastOutputTickMs = GetNowTickMs();
            if (!proc->logFilePathWide.empty()) {
                std::string inputLog = "\r\n[Input @ " + GetCurrentTimestampStr() + "] > " + text + "\r\n";
                AppendToLogFileWide(proc->logFilePathWide, inputLog);
            }
            WriteAuditLog("PROCESS_INPUT", "Sent input to #" + std::to_string(id) + " [" + proc->name + "]: \"" + JsonEscape(text) + "\"");
            return true;
        }
        outError = "ConPTY session is not active.";
        return false;
    }

    if (!proc->hStdinWrite) {
        outError = "Process ID " + std::to_string(id) + " does not have a writable stdin pipe.";
        return false;
    }

    std::string payload = text;
    if (appendNewline) payload += "\n";

    DWORD written = 0;
    if (!WriteFile(proc->hStdinWrite, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr)) {
        outError = "WriteFile to process stdin failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    FlushFileBuffers(proc->hStdinWrite);

    {
        std::lock_guard<std::mutex> outLock(proc->outputMutex);
        std::string echo = "\n> " + text + "\n";
        proc->cleanOutput += echo;
        proc->rawOutput += echo;
        proc->outputVersion++;
    }
    if (!proc->logFilePathWide.empty()) {
        std::string inputLog = "\r\n[Input @ " + GetCurrentTimestampStr() + "] > " + text + "\r\n";
        AppendToLogFileWide(proc->logFilePathWide, inputLog);
    }
    WriteAuditLog("PROCESS_INPUT", "Sent input to #" + std::to_string(id) + " [" + proc->name + "]: \"" + JsonEscape(text) + "\"");
    proc->lastOutputTickMs = GetNowTickMs();
    return true;
}

bool ProcessManager::KillProcess(int id, std::string& outError) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) {
        outError = "Process ID " + std::to_string(id) + " not found.";
        return false;
    }
    if (proc->state != ProcState::Running) {
        return true;
    }
    TerminateJobInternal(*proc, ProcState::Killed, "Process tree killed via Job Object.");
    SaveState();
    return true;
}

int ProcessManager::KillAllRunning() {
    std::vector<std::shared_ptr<ManagedProcess>> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = processes_;
    }
    int count = 0;
    for (auto& proc : copy) {
        if (proc->state == ProcState::Running) {
            TerminateJobInternal(*proc, ProcState::Killed, "Process tree killed (Kill All).");
            count++;
        }
    }
    if (count > 0) SaveState();
    return count;
}

int ProcessManager::ClearFinished() {
    std::vector<std::shared_ptr<ManagedProcess>> toDestroy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = processes_.begin();
        while (it != processes_.end()) {
            if ((*it)->state != ProcState::Running) {
                toDestroy.push_back(std::move(*it));
                it = processes_.erase(it);
            } else {
                ++it;
            }
        }
    }
    SaveState();
    return static_cast<int>(toDestroy.size());
}

bool ProcessManager::RemoveProcess(int id) {
    std::shared_ptr<ManagedProcess> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = processes_.begin(); it != processes_.end(); ++it) {
            if ((*it)->id == id && (*it)->state != ProcState::Running) {
                removed = std::move(*it);
                processes_.erase(it);
                break;
            }
        }
    }
    if (removed) {
        SaveState();
        return true;
    }
    return false;
}

int ProcessManager::ResetProcessIds(std::string& outError) {
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& proc : processes_) {
            if (proc->state == ProcState::Running) {
                outError = "Cannot reset process IDs while processes are running. Stop them first.";
                return -1;
            }
        }

        std::vector<std::shared_ptr<ManagedProcess>> ordered = processes_;
        std::sort(ordered.begin(), ordered.end(),
                  [](const std::shared_ptr<ManagedProcess>& a, const std::shared_ptr<ManagedProcess>& b) {
                      return a->id < b->id;
                  });

        int newId = 1;
        for (const auto& proc : ordered) {
            proc->id = newId++;
        }
        nextId_ = newId;
        count = static_cast<int>(ordered.size());
    }

    SaveState();
    return count;
}

struct FindWindowByPidData {
    DWORD targetPid;
    HWND foundHwnd;
};

static BOOL CALLBACK EnumWindowsByPidProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<FindWindowByPidData*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == data->targetPid && IsWindowVisible(hwnd)) {
        data->foundHwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

bool ProcessManager::FocusProcessWindow(int id) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) return false;
    if (proc->terminalView && proc->terminalView->Hwnd()) {
        SetFocus(proc->terminalView->Hwnd());
        return true;
    }
    if (proc->state != ProcState::Running) return false;

    FindWindowByPidData data{proc->rootPid, nullptr};
    EnumWindows(EnumWindowsByPidProc, reinterpret_cast<LPARAM>(&data));
    if (data.foundHwnd) {
        if (IsIconic(data.foundHwnd)) ShowWindow(data.foundHwnd, SW_RESTORE);
        SetForegroundWindow(data.foundHwnd);
        return true;
    }
    return false;
}

TerminalView* ProcessManager::GetTerminalView(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : processes_) {
        if (p->id == id) return p->terminalView;
    }
    return nullptr;
}

ProcessSnapshot ProcessManager::BuildSnapshot(ProcessManager::ManagedProcess& p, uint64_t now) {
    ProcessSnapshot snap;
    snap.id = p.id;
    snap.name = p.name;
    snap.commandLine = p.commandLine;
    snap.workingDir = p.workingDir;
    snap.mode = p.mode;
    snap.state = p.state.load();
    snap.rootPid = p.rootPid;
    snap.exitCode = p.exitCode.load();
     snap.startTimeStr = p.startTimeStr;
     snap.endTimeStr = p.endTimeStr;
     snap.startEpochSec = p.startEpochSec;
     snap.endEpochSec = p.endEpochSec;
    snap.estimatedEndTimeStr = p.estimatedEndTimeStr;
    snap.startTickMs = p.startTickMs;
    snap.endTickMs = p.endTickMs.load();
    snap.lastOutputTickMs = p.lastOutputTickMs.load();
    snap.lastCpuActivityTickMs = p.lastCpuActivityTickMs.load();
    snap.cpuTimeMs = p.cpuTime100ns.load() / 10000;
    if (snap.state == ProcState::Running) {
        snap.elapsedMs = (p.startTickMs > 0 && now >= p.startTickMs) ? (now - p.startTickMs) : p.restoredElapsedMs;
        uint64_t lastActivity = (std::max)(snap.lastOutputTickMs, snap.lastCpuActivityTickMs);
        snap.idleMs = (lastActivity > 0 && now >= lastActivity) ? (now - lastActivity) : 0;
    } else {
        if (p.startTickMs > 0 && snap.endTickMs >= p.startTickMs) {
            snap.elapsedMs = snap.endTickMs - p.startTickMs;
        } else {
            snap.elapsedMs = p.restoredElapsedMs;
        }
        snap.idleMs = 0;
    }
    snap.timeoutSec = p.timeoutSec;
    snap.idleTimeoutSec = p.idleTimeoutSec;
    snap.treeProcessCount = p.treeProcessCount.load();
    snap.treeMemoryBytes = p.treeMemoryBytes.load();
    snap.terminalHwnd = p.terminalView ? p.terminalView->Hwnd() : nullptr;
    snap.jobKey = p.jobKey;
    snap.jobId = p.jobId;
    snap.attemptN = p.attemptN;

    {
        std::lock_guard<std::mutex> outLock(p.outputMutex);
        snap.totalBytesRead = p.totalBytesRead;
        snap.outputVersion = p.outputVersion;
        snap.lastLinePreview = p.lastLinePreview;
        snap.logFilePath = p.logFilePath;
        snap.doneFilePath = p.doneFilePath;
    }

    if (p.mode == ExecMode::ConPTY && p.terminalView) {
        std::string termLast = WideToUtf8(p.terminalView->ExtractLastNonEmptyLine());
        if (!termLast.empty()) {
            if (termLast.size() > 120) termLast = termLast.substr(0, 117) + "...";
            snap.lastLinePreview = termLast;
        }
    }

    snap.agentType = p.agentType;
    snap.supervisorNotes = p.supervisorNotes;
    snap.supervisorGoal = p.supervisorGoal;
    snap.agentStatus = "idle";
    if (p.streamTracker) {
        LlmAgentState ast = p.streamTracker->GetState();
        snap.agentStatus = ast.status;
        snap.currentTool = ast.currentTool;
        snap.currentToolAction = ast.currentToolAction;
        snap.lastThought = ast.lastThought;
        snap.lastMessage = ast.lastMessage;
        snap.activity = ast.activity;
        snap.activityTarget = ast.activityTarget;
        snap.activitySinceEpochMs = ast.activitySinceEpochMs;
        snap.stepCount = ast.stepCount;
        snap.streamEventCount = ast.eventCount;
        snap.streamFilePath = ast.streamFilePath;
        if (!snap.currentTool.empty()) {
            std::string toolDesc = "Tool: " + snap.currentTool;
            if (!snap.currentToolAction.empty()) {
                toolDesc += " (" + snap.currentToolAction + ")";
            }
            snap.lastLinePreview = toolDesc;
        } else if (!snap.lastMessage.empty()) {
            snap.lastLinePreview = snap.lastMessage;
        } else if (!snap.lastThought.empty()) {
            snap.lastLinePreview = "Thinking: " + snap.lastThought;
        }
    }

    return snap;
}

bool ProcessManager::GetSnapshot(int id, ProcessSnapshot& outSnap) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) return false;
    outSnap = BuildSnapshot(*proc, GetNowTickMs());
    return true;
}

std::vector<ProcessSnapshot> ProcessManager::GetAllSnapshots() {
    std::vector<std::shared_ptr<ManagedProcess>> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = processes_;
    }
    uint64_t now = GetNowTickMs();
    std::vector<ProcessSnapshot> result;
    result.reserve(copy.size());
    for (auto& p : copy) {
        result.push_back(BuildSnapshot(*p, now));
    }
    return result;
}

bool ProcessManager::GetOutput(int id, bool stripAnsi, int tailLines, std::string& outText, uint64_t* outVersion) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) return false;

    {
        std::lock_guard<std::mutex> outLock(proc->outputMutex);
        if (outVersion) *outVersion = proc->outputVersion;
        if (!stripAnsi) {
            outText = (tailLines > 0) ? GetTailLines(proc->rawOutput, tailLines) : proc->rawOutput;
            return true;
        }
    }

    if (proc->mode == ExecMode::ConPTY && proc->terminalView) {
        std::wstring screenWide = proc->terminalView->ExtractScreenText(tailLines);
        outText = WideToUtf8(screenWide);
        if (outText.empty()) {
            std::lock_guard<std::mutex> outLock(proc->outputMutex);
            std::string cleanFallback = StripAnsiCodes(proc->rawOutput);
            outText = (tailLines > 0) ? GetTailLines(cleanFallback, tailLines) : cleanFallback;
        }
        return true;
    }

    {
        std::lock_guard<std::mutex> outLock(proc->outputMutex);
        outText = (tailLines > 0) ? GetTailLines(proc->cleanOutput, tailLines) : proc->cleanOutput;
    }
    if (outText.empty() && !proc->logFilePathWide.empty()) {
        std::string diskLog = ReadFileUtf8Wide(proc->logFilePathWide);
        if (!diskLog.empty()) {
            outText = (tailLines > 0) ? GetTailLines(diskLog, tailLines) : diskLog;
        }
    }
    return true;
}

bool ProcessManager::GetAgentStream(int id, int tail, const std::string& typeFilter, uint64_t sinceSeq,
                                    std::vector<LlmStreamEvent>& outEvents, LlmAgentState& outState) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) return false;

    if (proc->streamTracker) {
        outEvents = proc->streamTracker->GetEvents(tail, typeFilter, sinceSeq);
        outState = proc->streamTracker->GetState();
        return true;
    }

    std::wstring logsDirW = GetLogsDirectoryWide();
    std::string safeName = SanitizeFileNameComponent(proc->name);
    std::wstring streamPathW = logsDirW + L"\\proc_" + std::to_wstring(proc->id) + L"_" + Utf8ToWide(safeName) + L"_stream.jsonl";
    std::string content = ReadFileUtf8Wide(streamPathW);
    if (!content.empty()) {
        std::istringstream iss(content);
        std::string line;
        uint64_t seq = 1;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            std::vector<LlmStreamEvent> parsed;
            if (ParseLlmStreamLines(line, proc->agentType, parsed)) {
                for (auto& ev : parsed) {
                    ev.seq = seq++;
                    if (ev.seq > sinceSeq && (typeFilter.empty() || ev.type == typeFilter)) {
                        outEvents.push_back(ev);
                    }
                }
            }
        }
        if (tail > 0 && outEvents.size() > static_cast<size_t>(tail)) {
            outEvents.erase(outEvents.begin(), outEvents.end() - tail);
        }
        outState.agentType = proc->agentType;
        outState.streamFilePath = WideToUtf8(streamPathW);
        outState.eventCount = static_cast<int>(seq - 1);
        outState.status = (proc->state.load() == ProcState::Running) ? "idle" : "completed";
        return true;
    }
    return false;
}

bool ProcessManager::UpdateAgentInfo(int id, const std::string& notes, const std::string& goal) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!notes.empty()) proc->supervisorNotes = notes;
        if (!goal.empty()) proc->supervisorGoal = goal;
    }
    if (proc->streamTracker) {
        proc->streamTracker->UpdateSupervisorInfo(notes, goal);
    }
    return true;
}

bool ProcessManager::WaitForExit(int id, uint32_t maxWaitMs) {
    uint64_t start = GetNowTickMs();
    while (true) {
        ProcessSnapshot snap;
        if (!GetSnapshot(id, snap)) return true;
        if (snap.state != ProcState::Running) return true;
        if (maxWaitMs > 0 && (GetNowTickMs() - start) >= maxWaitMs) {
            return false;
        }
        Sleep(100);
    }
}

void ProcessManager::SaveState() {
    std::vector<ProcessSnapshot> snaps = GetAllSnapshots();
    int curNextId = 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        curNextId = nextId_;
    }

    std::ostringstream oss;
    oss << "{\r\n"
        << "  \"next_id\": " << curNextId << ",\r\n"
        << "  \"processes\": [\r\n";

    for (size_t i = 0; i < snaps.size(); ++i) {
        const auto& s = snaps[i];
        oss << "    {"
            << "\"id\":" << s.id << ","
            << "\"name\":\"" << JsonEscape(s.name) << "\","
            << "\"command\":\"" << JsonEscape(s.commandLine) << "\","
            << "\"cwd\":\"" << JsonEscape(s.workingDir) << "\","
            << "\"mode\":\"" << ExecModeToString(s.mode) << "\","
            << "\"state\":\"" << ProcStateToString(s.state) << "\","
            << "\"pid\":" << s.rootPid << ","
            << "\"exit_code\":" << s.exitCode << ","
            << "\"start_time\":\"" << JsonEscape(s.startTimeStr) << "\","
            << "\"end_time\":\"" << JsonEscape(s.endTimeStr) << "\","
            << "\"estimated_end_time\":\"" << JsonEscape(s.estimatedEndTimeStr) << "\","
            << "\"elapsed_ms\":" << s.elapsedMs << ","
            << "\"cpu_time_ms\":" << s.cpuTimeMs << ","
            << "\"timeout_sec\":" << s.timeoutSec << ","
            << "\"idle_timeout_sec\":" << s.idleTimeoutSec << ","
            << "\"total_bytes_read\":" << s.totalBytesRead << ","
            << "\"log_file\":\"" << JsonEscape(s.logFilePath) << "\","
            << "\"done_file\":\"" << JsonEscape(s.doneFilePath) << "\","
            << "\"last_line\":\"" << JsonEscape(s.lastLinePreview) << "\","
            << "\"job_key\":\"" << JsonEscape(s.jobKey) << "\","
            << "\"job_id\":" << s.jobId << ","
            << "\"attempt_n\":" << s.attemptN << ","
            << "\"agent_type\":\"" << JsonEscape(s.agentType) << "\","
            << "\"supervisor_notes\":\"" << JsonEscape(s.supervisorNotes) << "\","
            << "\"supervisor_goal\":\"" << JsonEscape(s.supervisorGoal) << "\""
            << "}" << (i + 1 < snaps.size() ? "," : "") << "\r\n";
    }
    oss << "  ]\r\n}\r\n";

    std::wstring statePath = GetLogsDirectoryWide() + L"\\processes_state.json";
    WriteFileAtomicWide(statePath, oss.str());
}

void ProcessManager::LoadState() {
    std::wstring statePath = GetLogsDirectoryWide() + L"\\processes_state.json";
    std::string content = ReadFileUtf8Wide(statePath);
    if (content.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    int64_t savedNextId = JsonGetInt(content, "next_id", 1);
    if (savedNextId > nextId_) {
        nextId_ = static_cast<int>(savedNextId);
    }

    size_t arrPos = content.find("\"processes\"");
    if (arrPos == std::string::npos) return;
    arrPos = content.find('[', arrPos);
    if (arrPos == std::string::npos) return;

    size_t pos = arrPos + 1;
    while (pos < content.size()) {
        size_t objStart = content.find('{', pos);
        if (objStart == std::string::npos) break;
        int depth = 0;
        bool inStr = false;
        size_t objEnd = std::string::npos;
        for (size_t i = objStart; i < content.size(); ++i) {
            char c = content[i];
            if (c == '"' && (i == 0 || content[i - 1] != '\\')) {
                inStr = !inStr;
            } else if (!inStr) {
                if (c == '{') depth++;
                else if (c == '}') {
                    depth--;
                    if (depth == 0) {
                        objEnd = i;
                        break;
                    }
                }
            }
        }
        if (objEnd == std::string::npos) break;

        std::string itemJson = content.substr(objStart, objEnd - objStart + 1);
        pos = objEnd + 1;

        int id = static_cast<int>(JsonGetInt(itemJson, "id", 0));
        if (id <= 0) continue;
        if (id >= nextId_) nextId_ = id + 1;

        auto proc = std::make_shared<ManagedProcess>();
        proc->id = id;
        proc->name = JsonGetString(itemJson, "name", "proc-" + std::to_string(id));
        proc->commandLine = JsonGetString(itemJson, "command", "");
        proc->workingDir = JsonGetString(itemJson, "cwd", "");
        proc->mode = StringToExecMode(JsonGetString(itemJson, "mode", "pty"));
        ProcState st = StringToProcState(JsonGetString(itemJson, "state", "completed"));
        if (st == ProcState::Running) {
            // Previous Hancho instance exited; Job Object closed and terminated child tree
            st = ProcState::Killed;
        }
        proc->state = st;
        proc->rootPid = static_cast<DWORD>(JsonGetInt(itemJson, "pid", 0));
        DWORD ec = static_cast<DWORD>(JsonGetInt(itemJson, "exit_code", 0));
        if (ec == STILL_ACTIVE) ec = 1;
        proc->exitCode = ec;
     proc->startTimeStr = JsonGetString(itemJson, "start_time", "");
     proc->startEpochSec = ParseTimestampStrToEpoch(proc->startTimeStr);
     proc->endTimeStr = JsonGetString(itemJson, "end_time", "");
     proc->endEpochSec = ParseTimestampStrToEpoch(proc->endTimeStr);
        proc->estimatedEndTimeStr = JsonGetString(itemJson, "estimated_end_time", "");
        proc->restoredElapsedMs = static_cast<uint64_t>(JsonGetInt(itemJson, "elapsed_ms", 0));
        proc->cpuTime100ns = static_cast<uint64_t>(JsonGetInt(itemJson, "cpu_time_ms", 0)) * 10000;
        proc->timeoutSec = static_cast<int>(JsonGetInt(itemJson, "timeout_sec", 0));
        proc->idleTimeoutSec = static_cast<int>(JsonGetInt(itemJson, "idle_timeout_sec", 0));
        proc->totalBytesRead = static_cast<uint64_t>(JsonGetInt(itemJson, "total_bytes_read", 0));
        proc->logFilePath = JsonGetString(itemJson, "log_file", "");
        proc->logFilePathWide = Utf8ToWide(proc->logFilePath);
        proc->doneFilePath = JsonGetString(itemJson, "done_file", "");
        proc->doneFilePathWide = Utf8ToWide(proc->doneFilePath);
        proc->lastLinePreview = JsonGetString(itemJson, "last_line", "");
        proc->jobKey = JsonGetString(itemJson, "job_key", "");
        proc->jobId = static_cast<int>(JsonGetInt(itemJson, "job_id", 0));
        proc->attemptN = static_cast<int>(JsonGetInt(itemJson, "attempt_n", 0));
        proc->agentType = JsonGetString(itemJson, "agent_type", "");
        proc->supervisorNotes = JsonGetString(itemJson, "supervisor_notes", "");
        proc->supervisorGoal = JsonGetString(itemJson, "supervisor_goal", "");

        processes_.push_back(proc);
    }
}

AgentExtractedResult ExtractAgentResult(ProcessManager& pm, int id, bool finished, int settleWaitMs) {
    constexpr size_t kMaxResultBytes = 64 * 1024;
    constexpr int kOutputFallbackLines = 50;

    AgentExtractedResult out;
    if (!pm.GetSnapshot(id, out.snap)) {
        return out;
    }
    out.found = true;

    // The monitor marks the process finished as soon as it exits, which can be before the reader
    // thread has ingested the last lines, and the last line is the one holding the result.
    if (finished && settleWaitMs > 0) {
        int stableTicks = 0;
        for (int i = 0; i < 30 && stableTicks < 3; ++i) {
            uint64_t bytesBefore = out.snap.totalBytesRead;
            Sleep(static_cast<DWORD>(settleWaitMs));
            pm.GetSnapshot(id, out.snap);
            stableTicks = (out.snap.totalBytesRead == bytesBefore) ? stableTicks + 1 : 0;
        }
    }

    std::vector<LlmStreamEvent> events;
    pm.GetAgentStream(id, 0, "", 0, events, out.agentState);

    if (finished) {
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->type != "result" && it->type != "error" && it->type != "message") continue;
            out.result = it->content;
            out.source = it->type;
            out.isError = it->type == "error";
            // Claude's result line can carry a short "summary" as content; the report is in "result".
            auto raw = nlohmann::json::parse(it->rawJson, nullptr, false);
            if (it->type != "message" && raw.is_object() && raw.contains("result") && raw["result"].is_string()) {
                out.result = raw["result"].get<std::string>();
            }
            break;
        }
        if (out.result.empty()) {
            pm.GetOutput(id, true, kOutputFallbackLines, out.result);
            out.source = "output";
        }
    }

    out.truncated = out.result.size() > kMaxResultBytes;
    if (out.truncated) {
        size_t cut = kMaxResultBytes;
        while (cut > 0 && (static_cast<unsigned char>(out.result[cut]) & 0xC0) == 0x80) cut--;
        out.result.resize(cut);
    }
    return out;
}

} // namespace pc
