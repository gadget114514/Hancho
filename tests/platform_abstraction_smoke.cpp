// Smoke test for the platform boundary (doc/design-v3.md 2.4).
//
// Uses only platform/platform.h: no Win32 or POSIX headers. The same test is
// built on Windows and POSIX so both implementations answer to one contract.

#include "platform/platform.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace pc::platform;

namespace {

int g_passes = 0;
int g_failures = 0;

void Check(bool ok, const std::string& label) {
    if (ok) {
        ++g_passes;
        std::printf("[PASS] %s\n", label.c_str());
    } else {
        ++g_failures;
        std::printf("[FAIL] %s\n", label.c_str());
    }
}

bool ReadExact(IIpcConnection& conn, void* buffer, size_t length, std::string& error) {
    char* p = static_cast<char*>(buffer);
    size_t left = length;
    while (left > 0) {
        int n = conn.Read(p, left, error);
        if (n <= 0) return false;
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

std::string UniqueEndpoint() {
#ifdef _WIN32
    return "\\\\.\\pipe\\hancho_platform_smoke_" + std::to_string(CurrentProcessId());
#else
    return "/tmp/hancho-platform-smoke-" + std::to_string(CurrentProcessId()) + ".sock";
#endif
}

} // namespace

int main() {
    // --- IShellQuote -------------------------------------------------------
#ifdef _WIN32
    Check(QuoteCommandArg("plain") == "\"plain\"", "windows argument is double-quoted");
    Check(QuoteCommandArg("a b") == "\"a b\"", "windows space is double-quoted");
    Check(QuoteCommandArg("a\\\"b") == "\"a\\\\\\\"b\"", "windows quote is escaped");
    Check(NeedsCommandArgQuoting("a b"), "windows whitespace needs quoting");
    Check(!NeedsCommandArgQuoting("plain"), "windows plain argument needs no quoting");
#else
    Check(QuoteCommandArg("plain") == "plain", "safe argument is not quoted");
    Check(QuoteCommandArg("a b") == "'a b'", "space is single-quoted");
    Check(QuoteCommandArg("it's") == "'it'\\''s'", "single quote is escaped");
    Check(NeedsCommandArgQuoting("a;b"), "semicolon needs quoting");
#endif

    // --- IInstanceLock -----------------------------------------------------
    {
        auto first = CreateInstanceLock("platform_abstraction_smoke");
        auto second = CreateInstanceLock("platform_abstraction_smoke");
        std::string error;
        Check(first && first->Acquire(error), "first instance lock is acquired: " + error);
        std::string secondError;
        Check(second && !second->Acquire(secondError), "second instance lock is refused");
        first->Release();
        std::string retryError;
        Check(second && second->Acquire(retryError), "lock is acquired after release: " + retryError);
        if (second) second->Release();
    }

    // --- IIpcTransport -----------------------------------------------------
    {
        const std::string endpoint = UniqueEndpoint();
        auto server = CreateIpcServer(endpoint);
        std::string error;
        Check(server && server->Start(error), "IPC server starts: " + error);

        std::thread worker([&] {
            std::string acceptError;
            auto conn = server->Accept(acceptError);
            if (!conn) return;
            char request[9] = {};
            if (!ReadExact(*conn, request, sizeof(request), acceptError)) return;
            const std::string reply = "echo:" + std::string(request, sizeof(request));
            conn->WriteAll(reply.data(), reply.size(), acceptError);
            conn->Close();
        });

        auto client = ConnectIpc(endpoint, 2000, error);
        Check(client != nullptr, "IPC client connects: " + error);
        if (client) {
            const std::string request = "ping-1234";
            Check(client->WriteAll(request.data(), request.size(), error), "IPC request is written");
            char response[14] = {};
            Check(ReadExact(*client, response, sizeof(response), error), "IPC response is read");
            Check(std::string(response, sizeof(response)) == "echo:" + request,
                  "IPC payload round-trips");
            client->Close();
        }
        server->Stop();
        worker.join();
    }

    // --- Helpers -----------------------------------------------------------
    Check(CurrentProcessId() != 0, "current process id is available");
    Check(!ToNativePath(L"a/b").empty(), "native path conversion returns a path");

    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_passes, g_failures);
    return g_failures ? 1 : 0;
}
