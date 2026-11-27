// Headless test for the Windows command line wrapper (src/win_command.h). The finder is faked, so
// the test does not depend on what is installed on the machine.

#include "../src/win_command.h"
#include <cstdio>
#include <string>

using namespace pc;

static int g_failures = 0;

static void Check(bool cond, const char* label) {
    std::printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", label);
    std::fflush(stdout);
    if (!cond) g_failures++;
}

// Only the files in `present` exist; each is looked up as name + ext.
static WinCommandFinder FinderWith(const std::wstring& present) {
    return [present](const std::wstring& name, const std::wstring& ext) -> std::wstring {
        std::wstring full = name + ext;
        return full == present ? full : L"";
    };
}

static WinCommandFinder NoFiles() {
    return [](const std::wstring&, const std::wstring&) -> std::wstring { return L""; };
}

int main() {
    const std::wstring npmClaude = L"C:\\Users\\x\\AppData\\Roaming\\npm\\claude.cmd";

    Check(WinCommandProgramToken(L"  claude -p hi") == L"claude", "program token: bare name");
    Check(WinCommandProgramToken(L"\"C:\\a b\\x.cmd\" -p") == L"C:\\a b\\x.cmd", "program token: quoted");
    Check(WinCommandProgramToken(L"") == L"", "program token: empty line");
    Check(WinPathExtension(L"C:\\v1.2\\tool") == L"", "extension: dot in directory is not an extension");
    Check(WinPathExtension(L"Claude.CMD") == L".cmd", "extension: lower-cased");

    // Bare name found only as claude.cmd on PATH -> wrapped.
    Check(WinCommandLineForCreate(L"claude -p hi", FinderWith(L"claude.cmd")) ==
              L"cmd.exe /d /s /c \"claude -p hi\"",
          "bare name that is a .cmd shim is wrapped");

    // Bare name that is a real .exe -> unchanged.
    Check(WinCommandLineForCreate(L"tool -x", FinderWith(L"tool.exe")) == L"tool -x",
          "bare name that is an .exe is unchanged");

    // Nothing found on PATH -> unchanged (CreateProcess will report the error as before).
    Check(WinCommandLineForCreate(L"missing -x", NoFiles()) == L"missing -x",
          "unknown program is unchanged");

    // Explicit .cmd path, quoted, with arguments -> wrapped without consulting the finder.
    Check(WinCommandLineForCreate(L"\"" + npmClaude + L"\" -p hi", NoFiles()) ==
              L"cmd.exe /d /s /c \"\"" + npmClaude + L"\" -p hi\"",
          "quoted explicit .cmd path is wrapped");

    // Explicit .exe path -> unchanged.
    Check(WinCommandLineForCreate(L"C:\\bin\\app.exe a", NoFiles()) == L"C:\\bin\\app.exe a",
          "explicit .exe path is unchanged");

    // Same name as both .exe and .cmd: PATHEXT order checks .exe first, so it stays unchanged.
    Check(WinCommandLineForCreate(L"both -a", FinderWith(L"both.exe")) == L"both -a",
          "exe is preferred when the same name also exists as .cmd");

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
