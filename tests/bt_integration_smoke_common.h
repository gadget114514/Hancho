#pragma once

#include "bt_manager.h"
#include "common.h"
#include "nlohmann/json.hpp"
#include "process_manager.h"
#include "taskqueue.h"

#include <windows.h>
#include <cstdio>
#include <string>

namespace bt_int_test {

using json = nlohmann::json;
using namespace pc;

inline int g_failures = 0;
inline int g_passes = 0;

inline void Check(bool cond, const char* msg) {
    if (cond) {
        g_passes++;
    } else {
        g_failures++;
        std::fprintf(stderr, "FAIL: %s\n", msg);
    }
}

inline void Check(bool cond, const std::string& msg) {
    Check(cond, msg.c_str());
}

inline void RemoveDirRecursive(const std::wstring& dir) {
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring full = dir + L"\\" + name;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                RemoveDirRecursive(full);
            } else {
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

inline bool FileExistsW(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

struct TestHarness {
    ProcessManager pm;
    TaskQueueManager tqm;
    std::unique_ptr<BtManager> btm;

    TestHarness() {
        tqm.SetProcessManager(&pm);
        pm.SetProcessFinishedCallback([this](int pid, int ec, const std::string& kind) {
            tqm.OnProcessFinished(pid, ec, kind);
        });
        btm = std::make_unique<BtManager>(pm, tqm);
    }

    ~TestHarness() {
        Shutdown();
    }

    void Shutdown() {
        if (btm) {
            btm->Shutdown();
            btm.reset();
        }
        pm.SetProcessFinishedCallback(nullptr);
        tqm.ShutdownTaskQueueManager();
    }
};

void RunPart2IntegrationTests(const std::wstring& tempDirW, const std::string& tempDirUtf8);

} // namespace bt_int_test
