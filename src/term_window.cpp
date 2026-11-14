#include "term_client.h"

#include "common.h"

#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace pc {

const std::wstring& TerminalClientWindowClass() {
    static const std::wstring name = [] {
        uint64_t hash = 1469598103934665603ULL;
        for (wchar_t c : GetEffectiveDataDirectoryWide()) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ULL;
        }
        wchar_t buf[64] = {};
        std::swprintf(buf, 64, L"HanchoTerminalClientWnd_%016llx",
                      static_cast<unsigned long long>(hash));
        return std::wstring(buf);
    }();
    return name;
}

} // namespace pc
