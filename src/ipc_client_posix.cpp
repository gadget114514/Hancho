#include "ipc_client.h"

#ifndef _WIN32

#include "common.h"
#include "platform/platform.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <vector>
#include <thread>
#include <unistd.h>

namespace pc {

namespace {

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string ResolveCoreExecutable() {
    std::string current = GetExecutablePathUtf8();
    std::filesystem::path dir = std::filesystem::path(current).parent_path();
    std::error_code ec;
    for (const char* name : {"Hancho", "hancho", "hanchod", "Hancho.exe"}) {
        std::filesystem::path candidate = dir / name;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate.string();
    }
    const char* path = std::getenv("PATH");
    if (path) {
        std::string p(path);
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(':', start);
            std::string part = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!part.empty()) {
                std::filesystem::path candidate = std::filesystem::path(part) / "Hancho";
                if (std::filesystem::is_regular_file(candidate, ec)) return candidate.string();
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    return "";
}

void SpawnCore() {
    std::string exe = ResolveCoreExecutable();
    if (exe.empty()) return;
    pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid == 0) {
        ::setsid();
        int devNull = ::open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            ::dup2(devNull, STDOUT_FILENO);
            ::dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) ::close(devNull);
        }
        std::vector<std::string> args = {exe, "--headless"};
        if (HasCustomDataDirectory()) {
            args.push_back("--data-dir");
            args.push_back(WideToUtf8(GetEffectiveDataDirectoryWide()));
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        ::execv(exe.c_str(), argv.data());
        _exit(127);
    }
}

} // namespace

bool SendIpcRequest(const std::string& reqJson, std::string& outRespJson, std::string& outError,
                    bool autoStartGui) {
    const std::string endpoint = GetIpcEndpointUtf8();
    std::string err;
    auto conn = platform::ConnectIpc(endpoint, 0, err);
    if (!conn && autoStartGui) {
        // A core holding the instance lock is still starting: wait, don't spawn another.
        if (!IsHanchoRunningForDataDirWide()) SpawnCore();
        for (int attempt = 0; attempt < 300 && !conn; ++attempt) {
            SleepMs(100);
            conn = platform::ConnectIpc(endpoint, 0, err);
        }
    }
    if (!conn) {
        outError = "Hancho core is not running" + (err.empty() ? std::string() : " (" + err + ")");
        return false;
    }

    if (!conn->WriteAll(reqJson.data(), reqJson.size(), err)) {
        outError = "Cannot write IPC request: " + err;
        return false;
    }
    conn->ShutdownWrite();

    outRespJson.clear();
    char buffer[16384];
    while (true) {
        int n = conn->Read(buffer, sizeof(buffer), err);
        if (n > 0) {
            outRespJson.append(buffer, static_cast<size_t>(n));
            continue;
        }
        break;
    }
    conn->Close();
    if (outRespJson.empty()) {
        outError = "Empty IPC response";
        return false;
    }
    return true;
}

} // namespace pc

#endif // !_WIN32
