#include "process_manager.h"

#ifndef _WIN32

#include "nlohmann/json.hpp"
#include "platform/platform.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace pc {

namespace {

std::vector<std::string> BuildEnvList(const std::map<std::string, std::string>& extra,
                                      const std::string& cwd) {
    std::vector<std::string> env;
    for (char** it = environ; it && *it; ++it) {
        std::string entry(*it);
        std::string key = entry.substr(0, entry.find('='));
        if (key == "PWD" || key == "OLDPWD" || key == "INIT_CWD") continue;
        bool overridden = false;
        for (const auto& kv : extra) {
            if (kv.first == key) {
                overridden = true;
                break;
            }
        }
        if (!overridden) env.push_back(entry);
    }
    if (!cwd.empty()) {
        env.push_back("PWD=" + cwd);
        env.push_back("INIT_CWD=" + cwd);
    }
    for (const auto& kv : extra) env.push_back(kv.first + "=" + kv.second);
    return env;
}

bool WaitExitNoHang(pid_t pid, int& outStatus) {
    int status = 0;
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) {
        outStatus = status;
        return true;
    }
    return false;
}

int DecodeExit(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

} // namespace

ProcessManager::ManagedProcess::~ManagedProcess() {
    if (stdinFd >= 0) {
        ::close(stdinFd);
        stdinFd = -1;
    }
    if (stdoutFd >= 0) {
        ::close(stdoutFd);
        stdoutFd = -1;
    }
    if (ptyFd >= 0) {
        ::close(ptyFd);
        ptyFd = -1;
    }
    eventPipeStop = true;
    if (eventServer) eventServer->Stop();
    if (eventPipeThread.joinable()) eventPipeThread.join();
    eventTracker.store(nullptr);
    if (readerThread.joinable()) readerThread.join();
    if (pty) {
        pty->Close();
        pty.reset();
    }
    if (tree) {
        tree->Release();
        tree.reset();
    }
}

ProcessManager::ProcessManager() {
    LoadState();
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
            SleepMs(200);
        }
    });
}

ProcessManager::~ProcessManager() {
    stopMonitor_ = true;
    if (monitorThread_.joinable()) monitorThread_.join();
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

    std::wstring cwdWide = ResolveAbsolutePathWide(Utf8ToWide(req.workingDir));
    const std::string cwdUtf8 = WideToUtf8(cwdWide);

    auto proc = std::make_shared<ManagedProcess>();
    proc->commandLine = req.commandLine;
    proc->workingDir = cwdUtf8;
    proc->mode = req.mode == ExecMode::Console ? ExecMode::Pipe : req.mode;
    proc->timeoutSec = req.timeoutSec;
    proc->idleTimeoutSec = req.idleTimeoutSec;
    proc->idleCpuSec = req.idleCpuSec;
    proc->jobKey = req.jobKey;
    proc->jobId = req.jobId;
    proc->attemptN = req.attemptN;
    proc->estimatedEndTimeStr = req.estimatedEndTimeStr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        proc->id = nextId_++;
    }
    if (req.name.empty()) proc->name = "proc-" + std::to_string(proc->id);
    else proc->name = req.name;

    const std::string safeName = SanitizeFileNameComponent(proc->name);
    const std::string fileTs = GetFileTimestampStr();
    const std::wstring logsDirW = GetLogsDirectoryWide();

    if (!req.logFile.empty()) {
        proc->logFilePathWide = Utf8ToWide(req.logFile);
        proc->logFilePath = req.logFile;
    } else {
        proc->logFilePathWide = logsDirW + L"/proc_" + Utf8ToWide(std::to_string(proc->id)) +
                                L"_" + Utf8ToWide(safeName) + L"_" + Utf8ToWide(fileTs) + L".log";
        proc->logFilePath = WideToUtf8(proc->logFilePathWide);
    }
    proc->defaultDoneFilePathWide = logsDirW + L"/proc_" + Utf8ToWide(std::to_string(proc->id)) + L".done";
    if (!req.doneFile.empty()) {
        proc->doneFilePathWide = Utf8ToWide(req.doneFile);
        proc->doneFilePath = req.doneFile;
    } else {
        proc->doneFilePathWide = proc->defaultDoneFilePathWide;
        proc->doneFilePath = WideToUtf8(proc->doneFilePathWide);
    }
    std::error_code rmEc;
    std::filesystem::remove(std::filesystem::path(WideToUtf8(ToNativePathWide(proc->defaultDoneFilePathWide))), rmEc);
    if (proc->doneFilePathWide != proc->defaultDoneFilePathWide) {
        std::filesystem::remove(std::filesystem::path(WideToUtf8(ToNativePathWide(proc->doneFilePathWide))), rmEc);
    }

    std::string detectedAgent = req.agentType;
    if (detectedAgent.empty() || detectedAgent == "auto") {
        detectedAgent = DetectAgentType(proc->commandLine, proc->name);
    }
    proc->agentType = detectedAgent;
    proc->jsonStream = req.jsonStream || !detectedAgent.empty();
    proc->supervisorNotes = req.supervisorNotes;
    proc->supervisorGoal = req.supervisorGoal;

    std::map<std::string, std::string> childEnv = req.env;
    if (proc->jsonStream && childEnv.find("HANCHO_EVENT_PIPE") == childEnv.end()) {
        const std::string endpoint = WideToUtf8(logsDirW) + "/proc_" + std::to_string(proc->id) + "_events.sock";
        auto eventServer = platform::CreateIpcServer(endpoint);
        std::string eventErr;
        if (eventServer && eventServer->Start(eventErr)) {
            childEnv["HANCHO_EVENT_PIPE"] = endpoint;
            std::shared_ptr<platform::IIpcServer> server = std::move(eventServer);
            proc->eventEndpoint = endpoint;
            proc->eventServer = server;
            proc->eventPipeThread = std::thread([proc, server] {
                while (!proc->eventPipeStop.load()) {
                    std::string err;
                    auto conn = server->Accept(err);
                    if (!conn) {
                        if (proc->eventPipeStop.load()) break;
                        SleepMs(20);
                        continue;
                    }
                    std::string buffer;
                    std::vector<std::string> backlog;
                    char chunk[4096];
                    while (!proc->eventPipeStop.load()) {
                        int n = conn->Read(chunk, sizeof(chunk), err);
                        if (n <= 0) break;
                        buffer.append(chunk, static_cast<size_t>(n));
                        size_t pos = 0;
                        while ((pos = buffer.find('\n')) != std::string::npos) {
                            std::string line = buffer.substr(0, pos);
                            buffer.erase(0, pos + 1);
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            if (line.empty()) continue;
                            LlmStreamTracker* tracker = proc->eventTracker.load();
                            if (tracker) tracker->IngestExternalEventLine(line);
                            else if (backlog.size() < 1000) backlog.push_back(std::move(line));
                        }
                        LlmStreamTracker* tracker = proc->eventTracker.load();
                        if (tracker && !backlog.empty()) {
                            for (const auto& line : backlog) tracker->IngestExternalEventLine(line);
                            backlog.clear();
                        }
                    }
                }
                server->Stop();
            });
        }
    }

    proc->startTimeStr = GetCurrentTimestampStr();
    proc->startEpochSec = static_cast<int64_t>(time(nullptr));
    const uint64_t now = GetNowTickMs();
    proc->startTickMs = now;
    proc->lastOutputTickMs = now;
    proc->lastCpuActivityTickMs = now;

    if (proc->mode == ExecMode::ConPTY) {
        platform::PtySpawnOptions opts;
        opts.command = req.commandLine;
        opts.workingDir = cwdUtf8;
        opts.env = BuildEnvList(childEnv, cwdUtf8);
        std::string spawnErr;
        proc->pty = platform::SpawnPty(opts, spawnErr);
        if (!proc->pty) {
            outError = spawnErr.empty() ? "forkpty failed" : spawnErr;
            return -1;
        }
        proc->rootPid = static_cast<DWORD>(proc->pty->Pid());
        proc->ptyFd = -1;
        proc->ptyMode = true;
    } else {
        int inPipe[2] = {-1, -1};
        int outPipe[2] = {-1, -1};
        if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0) {
            if (inPipe[0] >= 0) { ::close(inPipe[0]); ::close(inPipe[1]); }
            if (outPipe[0] >= 0) { ::close(outPipe[0]); ::close(outPipe[1]); }
            outError = std::string("pipe failed: ") + std::strerror(errno);
            return -1;
        }
        pid_t pid = ::fork();
        if (pid < 0) {
            ::close(inPipe[0]); ::close(inPipe[1]);
            ::close(outPipe[0]); ::close(outPipe[1]);
            outError = std::string("fork failed: ") + std::strerror(errno);
            return -1;
        }
        if (pid == 0) {
            ::setsid();
            if (!cwdUtf8.empty() && ::chdir(cwdUtf8.c_str()) != 0) _exit(127);
            for (const auto& kv : childEnv) ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
            if (!cwdUtf8.empty()) {
                ::setenv("PWD", cwdUtf8.c_str(), 1);
                ::setenv("INIT_CWD", cwdUtf8.c_str(), 1);
            }
            ::dup2(inPipe[0], STDIN_FILENO);
            ::dup2(outPipe[1], STDOUT_FILENO);
            ::dup2(outPipe[1], STDERR_FILENO);
            ::close(inPipe[0]); ::close(inPipe[1]);
            ::close(outPipe[0]); ::close(outPipe[1]);
            ::execl("/bin/sh", "sh", "-c", req.commandLine.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        ::close(inPipe[0]);
        ::close(outPipe[1]);
        proc->stdinFd = inPipe[1];
        proc->stdoutFd = outPipe[0];
        proc->rootPid = static_cast<DWORD>(pid);
        proc->ptyMode = false;
    }

    if (req.openStdin == false && proc->stdinFd >= 0) {
        ::close(proc->stdinFd);
        proc->stdinFd = -1;
    } else if (req.openStdin == false && proc->pty) {
        // The PTY stays open for output; input is simply not written.
    }

    proc->tree = platform::CreateProcessTree();
    if (proc->tree) {
        std::string attachErr;
        proc->tree->Attach(proc->rootPid, attachErr);
    }
    proc->treeProcessCount = 1;

    if (proc->jsonStream) {
        std::wstring streamPathW = logsDirW + L"/proc_" + Utf8ToWide(std::to_string(proc->id)) +
                                   L"_" + Utf8ToWide(safeName) + L"_stream.jsonl";
        proc->streamTracker = std::make_unique<LlmStreamTracker>();
        proc->streamTracker->Init(proc->id, proc->agentType, WideToUtf8(streamPathW),
                                  proc->supervisorNotes, proc->supervisorGoal, "",
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
        AppendToLogFileWide(proc->logFilePathWide, hdr.str());
    }

    WriteAuditLog("PROCESS_START",
                  "Started #" + std::to_string(proc->id) + " [" + proc->name + "] PID=" +
                      std::to_string(proc->rootPid) + " Mode=" + ExecModeToString(proc->mode) +
                      " Cmd=\"" + proc->commandLine + "\" Log=\"" + proc->logFilePath + "\"");

    proc->readerThread = std::thread([this, proc] {
        try {
            ReaderThreadFunc(proc);
        } catch (const std::exception& e) {
            WriteAuditLog("CRASH_GUARD", "Reader thread exception (#" + std::to_string(proc->id) + "): " + e.what());
        } catch (...) {
            WriteAuditLog("CRASH_GUARD", "Reader thread exception (#" + std::to_string(proc->id) + "): unknown");
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        processes_.push_back(proc);
    }
    SaveState();
    return proc->id;
}

void ProcessManager::ReaderThreadFunc(std::shared_ptr<ManagedProcess> proc) {
    char buffer[4096];
    while (true) {
        int n = 0;
        if (proc->pty) {
            n = proc->pty->Read(buffer, sizeof(buffer));
        } else if (proc->stdoutFd >= 0) {
            n = static_cast<int>(::read(proc->stdoutFd, buffer, sizeof(buffer)));
            if (n < 0 && errno == EINTR) continue;
        } else {
            break;
        }
        if (n <= 0) break;

        proc->lastOutputTickMs = GetNowTickMs();
        std::string chunk(buffer, static_cast<size_t>(n));
        std::string cleanChunk = StripAnsiCodes(chunk);
        std::wstring logPathW;
        {
            std::lock_guard<std::mutex> outLock(proc->outputMutex);
            proc->rawOutput.append(chunk);
            proc->cleanOutput.append(cleanChunk);
            proc->totalBytesRead += static_cast<uint64_t>(n);
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
            if (!preview.empty()) proc->lastLinePreview = preview;
        }
        if (!logPathW.empty() && !cleanChunk.empty()) {
            AppendToLogFileWide(logPathW, cleanChunk);
        }
        if (proc->streamTracker && !cleanChunk.empty()) {
            proc->streamTracker->IngestChunk(cleanChunk.data(), cleanChunk.size());
        }
    }
}

void ProcessManager::EventPipeThreadFunc(std::shared_ptr<ManagedProcess> proc) {
    (void)proc;
}

void ProcessManager::UpdateJobMetrics(ManagedProcess& proc) {
    if (!proc.tree) return;
    platform::ProcessMetrics metrics;
    if (proc.tree->Query(metrics)) {
        proc.treeProcessCount = metrics.processCount;
        proc.treeMemoryBytes = metrics.memoryBytes;
        const uint64_t new100ns = metrics.cpuTimeMs * 10000ull;
        const uint64_t prev = proc.cpuTime100ns.exchange(new100ns);
        if (new100ns > prev) proc.lastCpuActivityTickMs = GetNowTickMs();
    }
}

void ProcessManager::MonitorThreadFunc() {
    while (!stopMonitor_) {
        std::vector<std::shared_ptr<ManagedProcess>> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            copy = processes_;
        }

        const uint64_t now = GetNowTickMs();
        for (auto& proc : copy) {
            if (proc->state != ProcState::Running) continue;

            int status = 0;
            bool exited = false;
            if (!proc->reaped && proc->rootPid > 0 && WaitExitNoHang(static_cast<pid_t>(proc->rootPid), status)) {
                proc->reaped = true;
                proc->exitStatus = status;
                exited = true;
            }

            if (exited) {
                if (proc->tree) proc->tree->Kill(0);
                int code = DecodeExit(status);
                proc->exitCode = code;
                proc->endTickMs = now;
                ProcState finalSt = (code == 0) ? ProcState::Completed : ProcState::Failed;
                proc->state = finalSt;
                if (proc->stdinFd >= 0) {
                    ::close(proc->stdinFd);
                    proc->stdinFd = -1;
                }
                proc->treeProcessCount = 0;
                proc->treeMemoryBytes = 0;

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
                        << "Done File   : " << proc->doneFilePath << "\r\n"
                        << "================================================================================\r\n";
                    AppendToLogFileWide(proc->logFilePathWide, ftr.str());
                }

                WriteDoneFile(*proc, finalSt, code, endTimeStr, elapsedMs, "Exited normally");
                WriteAuditLog("PROCESS_EXIT",
                              "Process #" + std::to_string(proc->id) + " [" + proc->name + "] exited with code " +
                                  std::to_string(code) + " Uptime=" + FormatUptimeHMS(elapsedMs));
                NotifyProcessFinished(*proc, code, ProcStateToString(finalSt));
                SaveState();
                continue;
            }

            UpdateJobMetrics(*proc);

            if (proc->timeoutSec > 0 && (now - proc->startTickMs) / 1000 >= static_cast<uint64_t>(proc->timeoutSec)) {
                TerminateJobInternal(*proc, ProcState::TimedOut,
                    "Process tree terminated: Exceeded runtime timeout (" + std::to_string(proc->timeoutSec) + "s)");
                SaveState();
                continue;
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
        SleepMs(200);
    }
}

bool ProcessManager::SendInput(int id, const std::string& text, bool appendNewline, std::string& outError) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) if (p->id == id) { proc = p; break; }
    }
    if (!proc) {
        outError = "Process ID not found.";
        return false;
    }
    std::string payload = text;
    if (appendNewline && (payload.empty() || payload.back() != '\n')) payload.push_back('\n');

    bool ok = false;
    if (proc->pty) {
        ok = proc->pty->Write(payload.data(), payload.size());
    } else if (proc->stdinFd >= 0) {
        size_t left = payload.size();
        const char* p = payload.data();
        ok = true;
        while (left > 0) {
            ssize_t n = ::write(proc->stdinFd, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            p += n;
            left -= static_cast<size_t>(n);
        }
    }
    if (!ok) {
        outError = "Cannot write to process stdin.";
        return false;
    }
    return true;
}

void ProcessManager::TerminateJobInternal(ManagedProcess& proc, ProcState finalState,
                                          const std::string& reasonMsg) {
    if (proc.tree) proc.tree->Kill(1);
    else if (proc.rootPid > 0) ::kill(-static_cast<pid_t>(proc.rootPid), SIGKILL);
    if (proc.stdinFd >= 0) {
        ::close(proc.stdinFd);
        proc.stdinFd = -1;
    }
    if (!proc.reaped && proc.rootPid > 0) {
        int status = 0;
        while (::waitpid(static_cast<pid_t>(proc.rootPid), &status, 0) < 0) {
            if (errno != EINTR) break;
        }
        proc.exitStatus = status;
        proc.reaped = true;
    }

    const uint64_t endNow = GetNowTickMs();
    proc.endTickMs = endNow;
    proc.exitCode = 1;
    proc.state = finalState;
    proc.treeProcessCount = 0;
    proc.treeMemoryBytes = 0;
    proc.endTimeStr = GetCurrentTimestampStr();
    proc.endEpochSec = static_cast<int64_t>(time(nullptr));
    uint64_t elapsedMs = (endNow >= proc.startTickMs) ? (endNow - proc.startTickMs) : 0;
    proc.restoredElapsedMs = elapsedMs;

    WriteDoneFile(proc, finalState, 1, proc.endTimeStr, elapsedMs, reasonMsg);
    WriteAuditLog("PROCESS_TERMINATE",
                  "Process #" + std::to_string(proc.id) + " [" + proc.name + "] " +
                      ProcStateToString(finalState) + ": " + reasonMsg);
    NotifyProcessFinished(proc, 1, ProcStateToString(finalState));
}

bool ProcessManager::KillProcess(int id, std::string& outError) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) if (p->id == id) { proc = p; break; }
    }
    if (!proc) {
        outError = "Process ID not found.";
        return false;
    }
    if (proc->state == ProcState::Running) {
        TerminateJobInternal(*proc, ProcState::Killed, "Process tree killed by request.");
        SaveState();
    }
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
        if (proc->state != ProcState::Running) continue;
        TerminateJobInternal(*proc, ProcState::Killed, "Process tree killed (Kill All).");
        ++count;
    }
    if (count) SaveState();
    return count;
}

int ProcessManager::ClearFinished() {
    int removed = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = processes_.begin(); it != processes_.end();) {
            if ((*it)->state != ProcState::Running) {
                it = processes_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    }
    if (removed) SaveState();
    return removed;
}

bool ProcessManager::RemoveProcess(int id) {
    std::shared_ptr<ManagedProcess> found;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = processes_.begin(); it != processes_.end(); ++it) {
            if ((*it)->id == id) {
                found = *it;
                processes_.erase(it);
                break;
            }
        }
    }
    if (!found) return false;
    if (found->state == ProcState::Running) {
        TerminateJobInternal(*found, ProcState::Killed, "Process removed.");
    }
    SaveState();
    return true;
}

int ProcessManager::ResetProcessIds(std::string& outError) {
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& p : processes_) {
            if (p->state == ProcState::Running) {
                outError = "Cannot reset process IDs while processes are running. Stop them first.";
                return -1;
            }
        }
        int next = 1;
        for (auto& p : processes_) p->id = next++;
        nextId_ = next;
        count = next - 1;
    }
    SaveState();
    return count;
}

bool ProcessManager::FocusProcessWindow(int id) {
    (void)id;
    return false;
}

TerminalView* ProcessManager::GetTerminalView(int id) {
    (void)id;
    return nullptr;
}

ProcessSnapshot ProcessManager::BuildSnapshot(ManagedProcess& p, uint64_t now) {
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
        snap.elapsedMs = (p.startTickMs > 0 && snap.endTickMs >= p.startTickMs)
                             ? (snap.endTickMs - p.startTickMs)
                             : p.restoredElapsedMs;
        snap.idleMs = 0;
    }
    snap.timeoutSec = p.timeoutSec;
    snap.idleTimeoutSec = p.idleTimeoutSec;
    snap.treeProcessCount = p.treeProcessCount.load();
    snap.treeMemoryBytes = p.treeMemoryBytes.load();
    snap.terminalHwnd = nullptr;
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
            snap.lastLinePreview = "Tool: " + snap.currentTool;
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
        for (auto& p : processes_) if (p->id == id) { proc = p; break; }
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
    const uint64_t now = GetNowTickMs();
    std::vector<ProcessSnapshot> result;
    result.reserve(copy.size());
    for (auto& p : copy) result.push_back(BuildSnapshot(*p, now));
    return result;
}

bool ProcessManager::GetOutput(int id, bool stripAnsi, int tailLines, std::string& outText,
                               uint64_t* outVersion) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) if (p->id == id) { proc = p; break; }
    }
    if (!proc) return false;
    {
        std::lock_guard<std::mutex> outLock(proc->outputMutex);
        if (outVersion) *outVersion = proc->outputVersion;
        const std::string& source = stripAnsi ? proc->cleanOutput : proc->rawOutput;
        outText = (tailLines > 0) ? GetTailLines(source, tailLines) : source;
    }
    if (outText.empty() && !proc->logFilePathWide.empty()) {
        std::string diskLog = ReadFileUtf8Wide(proc->logFilePathWide);
        if (!diskLog.empty()) {
            outText = (tailLines > 0) ? GetTailLines(diskLog, tailLines) : diskLog;
        }
    }
    return true;
}

bool ProcessManager::GetAgentStream(int id, int tail, const std::string& typeFilter,
                                    uint64_t sinceSeq, std::vector<LlmStreamEvent>& outEvents,
                                    LlmAgentState& outState) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) if (p->id == id) { proc = p; break; }
    }
    if (!proc) return false;
    if (proc->streamTracker) {
        outEvents = proc->streamTracker->GetEvents(tail, typeFilter, sinceSeq);
        outState = proc->streamTracker->GetState();
        return true;
    }
    outState.agentType = proc->agentType;
    outState.status = (proc->state.load() == ProcState::Running) ? "idle" : "completed";
    return false;
}

bool ProcessManager::UpdateAgentInfo(int id, const std::string& notes, const std::string& goal) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) if (p->id == id) { proc = p; break; }
    }
    if (!proc) return false;
    if (!notes.empty()) proc->supervisorNotes = notes;
    if (!goal.empty()) proc->supervisorGoal = goal;
    if (proc->streamTracker) proc->streamTracker->UpdateSupervisorInfo(notes, goal);
    return true;
}

bool ProcessManager::WaitForExit(int id, uint32_t maxWaitMs) {
    const uint64_t start = GetNowTickMs();
    while (true) {
        ProcessSnapshot snap;
        if (!GetSnapshot(id, snap)) return true;
        if (snap.state != ProcState::Running) return true;
        if (maxWaitMs > 0 && (GetNowTickMs() - start) >= maxWaitMs) return false;
        SleepMs(100);
    }
}

void ProcessManager::WriteDoneFile(ManagedProcess& proc, ProcState finalState, int exitCode,
                                   const std::string& endTimeStr, uint64_t elapsedMs,
                                   const std::string& reasonMsg) {
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
    const std::string payload = oss.str();
    if (!proc.defaultDoneFilePathWide.empty()) WriteFileAtomicWide(proc.defaultDoneFilePathWide, payload);
    if (!proc.doneFilePathWide.empty() && proc.doneFilePathWide != proc.defaultDoneFilePathWide) {
        WriteFileAtomicWide(proc.doneFilePathWide, payload);
    }
}

void ProcessManager::NotifyProcessFinished(ManagedProcess& proc, int exitCode,
                                           const std::string& endKind) {
    ProcessFinishedCallback cb;
    {
        std::lock_guard<std::mutex> lock(cbMutex_);
        cb = finishedCb_;
    }
    if (cb) cb(proc.id, exitCode, endKind);
}

void ProcessManager::SaveState() {
    std::vector<ProcessSnapshot> snaps = GetAllSnapshots();
    int curNextId = 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        curNextId = nextId_;
    }
    std::ostringstream oss;
    oss << "{\r\n  \"next_id\": " << curNextId << ",\r\n  \"processes\": [\r\n";
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
    WriteFileAtomicWide(GetLogsDirectoryWide() + L"/processes_state.json", oss.str());
}

void ProcessManager::LoadState() {
    std::wstring statePath = GetLogsDirectoryWide() + L"/processes_state.json";
    std::string content = ReadFileUtf8Wide(statePath);
    if (content.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    int64_t savedNextId = JsonGetInt(content, "next_id", 1);
    if (savedNextId > nextId_) nextId_ = static_cast<int>(savedNextId);

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
            if (c == '"' && (i == 0 || content[i - 1] != '\\')) inStr = !inStr;
            else if (!inStr) {
                if (c == '{') depth++;
                else if (c == '}' && --depth == 0) { objEnd = i; break; }
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
        proc->mode = StringToExecMode(JsonGetString(itemJson, "mode", "pipe"));
        ProcState st = StringToProcState(JsonGetString(itemJson, "state", "completed"));
        if (st == ProcState::Running) st = ProcState::Killed;
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
    if (!pm.GetSnapshot(id, out.snap)) return out;
    out.found = true;

    if (finished && settleWaitMs > 0) {
        int stableTicks = 0;
        for (int i = 0; i < 30 && stableTicks < 3; ++i) {
            uint64_t bytesBefore = out.snap.totalBytesRead;
            SleepMs(static_cast<uint32_t>(settleWaitMs));
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
            auto raw = nlohmann::json::parse(it->rawJson, nullptr, false);
            if (it->type != "message" && raw.is_object() && raw.contains("result") &&
                raw["result"].is_string()) {
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

#endif // !_WIN32
