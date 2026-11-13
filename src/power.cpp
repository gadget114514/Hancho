#include "power.h"

#include <string>

#ifdef _WIN32
#include <windows.h>
#include <powrprof.h>

namespace {

// SetSuspendState requires SE_SHUTDOWN_NAME. An administrator token usually has it,
// but it is disabled by default, so enable it for this call.
bool EnableShutdownPrivilege(std::string& outError) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        outError = "OpenProcessToken failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    LUID luid = {};
    if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &luid)) {
        DWORD err = GetLastError();
        CloseHandle(token);
        outError = "LookupPrivilegeValue(SE_SHUTDOWN_NAME) failed (error " +
                   std::to_string(err) + ")";
        return false;
    }

    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) ||
        GetLastError() != ERROR_SUCCESS) {
        DWORD err = GetLastError();
        CloseHandle(token);
        if (err == ERROR_NOT_ALL_ASSIGNED) {
            outError = "SE_SHUTDOWN_NAME is not held by this process; run Hancho elevated "
                       "to allow suspend";
        } else {
            outError = "AdjustTokenPrivileges failed (error " + std::to_string(err) + ")";
        }
        return false;
    }

    CloseHandle(token);
    return true;
}

} // namespace

namespace pc {

bool SuspendSystem(bool hibernate, std::string& outError) {
    outError.clear();
    if (!EnableShutdownPrivilege(outError)) return false;

    if (!SetSuspendState(hibernate ? TRUE : FALSE, FALSE, FALSE)) {
        DWORD err = GetLastError();
        outError = "SetSuspendState failed (error " + std::to_string(err) + ")";
        if (err == ERROR_PRIVILEGE_NOT_HELD) {
            outError += "; SE_SHUTDOWN_NAME is not held by this process";
        }
        return false;
    }
    return true;
}

} // namespace pc

#else

#include <cstdlib>

namespace pc {

bool SuspendSystem(bool hibernate, std::string& outError) {
    outError.clear();
    const char* command = hibernate ? "systemctl hibernate" : "systemctl suspend";
    int rc = std::system(command);
    if (rc != 0) {
        outError = std::string("command failed: ") + command;
        return false;
    }
    return true;
}

} // namespace pc

#endif
