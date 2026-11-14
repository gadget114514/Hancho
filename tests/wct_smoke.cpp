// End-to-end smoke test for wct.exe (src/wct_client.cpp).
//
// wct runs inside a local ConPTY, connects to an isolated Hancho core, starts a
// `cmd` session over WCSP, forwards a command, returns the output, then closes
// the session with Ctrl+\. The test only checks observable console traffic.
//
// Usage:
//   wct_smoke --wct <wct.exe> --data-dir <isolated data dir>
//
// The isolated core must already be running with the terminal server enabled.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (ok) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                        out.data(), need, nullptr, nullptr);
    return out;
}

struct ConPtyHost {
    HPCON pty = nullptr;
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION process{};
    std::thread reader;
    std::mutex mutex;
    std::condition_variable cv;
    std::string text;

    ~ConPtyHost() { Close(); }

    bool Start(const std::wstring& command) {
        HANDLE inputRead = INVALID_HANDLE_VALUE;
        HANDLE outputWrite = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&inputRead, &input, nullptr, 0) ||
            !CreatePipe(&output, &outputWrite, nullptr, 0)) {
            std::fprintf(stderr, "FAIL: CreatePipe: %lu\n", GetLastError());
            return false;
        }
        if (FAILED(CreatePseudoConsole(COORD{100, 30}, inputRead, outputWrite, 0, &pty))) {
            std::fprintf(stderr, "FAIL: CreatePseudoConsole: %lu\n", GetLastError());
            return false;
        }
        CloseHandle(inputRead);
        CloseHandle(outputWrite);

        STARTUPINFOEXW siEx{};
        siEx.StartupInfo.cb = sizeof(siEx);
        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        siEx.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, attributeSize));
        if (!siEx.lpAttributeList ||
            !InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attributeSize) ||
            !UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                                       PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       pty, sizeof(pty), nullptr, nullptr)) {
            std::fprintf(stderr, "FAIL: attribute list: %lu\n", GetLastError());
            return false;
        }

        std::wstring mutableCommand = command;
        BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
                                 FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                 &siEx.StartupInfo, &process);
        DeleteProcThreadAttributeList(siEx.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        if (!ok) {
            std::fprintf(stderr, "FAIL: CreateProcessW: %lu\n", GetLastError());
            return false;
        }

        reader = std::thread([this] {
            char buffer[4096];
            DWORD read = 0;
            while (ReadFile(output, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
                std::lock_guard<std::mutex> lock(mutex);
                text.append(buffer, read);
                cv.notify_all();
            }
        });
        return true;
    }

    bool WaitFor(const std::string& needle, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        std::unique_lock<std::mutex> lock(mutex);
        while (true) {
            if (text.find(needle) != std::string::npos) return true;
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                return text.find(needle) != std::string::npos;
            }
        }
    }

    bool Write(const std::string& bytes) {
        DWORD written = 0;
        return WriteFile(input, bytes.data(), static_cast<DWORD>(bytes.size()),
                         &written, nullptr) && written == bytes.size();
    }

    void Close() {
        if (process.hProcess) {
            WaitForSingleObject(process.hProcess, 10000);
            TerminateProcess(process.hProcess, 0);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            process = {};
        }
        if (pty) {
            ClosePseudoConsole(pty);
            pty = nullptr;
        }
        if (reader.joinable()) reader.join();
        if (input != INVALID_HANDLE_VALUE) {
            CloseHandle(input);
            input = INVALID_HANDLE_VALUE;
        }
        if (output != INVALID_HANDLE_VALUE) {
            CloseHandle(output);
            output = INVALID_HANDLE_VALUE;
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    std::wstring wctPath;
    std::wstring dataDir;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](std::wstring& out) -> bool {
            if (i + 1 >= argc) return false;
            std::string value = argv[++i];
            int need = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0);
            out.assign((size_t)need, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), out.data(), need);
            return true;
        };
        if (arg == "--wct") {
            if (!next(wctPath)) return 2;
        } else if (arg == "--data-dir") {
            if (!next(dataDir)) return 2;
        }
    }
    if (wctPath.empty() || dataDir.empty()) {
        std::fprintf(stderr, "usage: wct_smoke --wct <wct.exe> --data-dir <dir>\n");
        return 2;
    }

    // The test harness itself may have a console (when run from a terminal).
    // Detach from it so the child is attached only to the pseudo console and
    // receives the pty handles as its standard handles.
    FreeConsole();

    ConPtyHost host;
    const std::wstring command = L"\"" + wctPath + L"\" --data-dir \"" + dataDir +
                                 L"\" --shell cmd";
    if (!host.Start(command)) return 1;

    Check(host.WaitFor("Ctrl+] detaches", 15000), "wct prints the connect hint");

    // The command text does not contain the result, so the match proves that the
    // remote command actually ran (not just the echoed input).
    host.Write("set /a 1234567+1\r");
    Check(host.WaitFor("1234568", 15000), "remote cmd output round-trips through wct");

    host.Write(std::string(1, '\x1C')); // Ctrl+backslash
    Check(host.WaitFor("[wct] session closed", 15000) ||
          host.WaitFor("[wct] session exited", 1000),
          "wct closes the session after Ctrl+\\");

    if (g_failures != 0) {
        std::lock_guard<std::mutex> lock(host.mutex);
        std::fprintf(stderr, "---- wct console output ----\n%s\n---------------------------\n",
                     host.text.c_str());
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("wct_smoke: all checks passed\n");
    return 0;
}
