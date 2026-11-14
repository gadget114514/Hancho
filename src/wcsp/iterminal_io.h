#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Backend interface used by TerminalView.  LocalSession wraps ConPTY;
// RemoteSession talks to WinConsoleServer.
struct TerminalStartOptions {
    std::wstring shell;                 // local: command line, remote: requested shell name
    int cols = 80;
    int rows = 24;
    UINT codePage = CP_UTF8;
    std::vector<std::pair<std::wstring, std::wstring>> env;
};

class ITerminalIo {
public:
    using OutputFn = std::function<void(const char*, size_t)>;

    virtual ~ITerminalIo() = default;

    virtual void SetOutputSink(OutputFn fn) = 0;
    virtual bool Start(const TerminalStartOptions& opt, std::wstring* error) = 0;

    virtual void Write(const void* data, size_t len) = 0;
    virtual void WriteFront(const void* data, size_t len) = 0;
    virtual void Resize(int cols, int rows) = 0;

    virtual bool IsRunning() const = 0;
    virtual void Close() = 0;
    virtual std::wstring LastError() const = 0;
};
