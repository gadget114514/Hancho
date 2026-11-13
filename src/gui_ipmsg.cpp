// Chat dialog (IPMsg compatible protocol). Standalone Win32 window that talks to the
// Hancho core over the Named Pipe IPC (chat_* actions). It can be launched from
// the dashboard toolbar (`Hancho.exe chat`) or directly and runs without the
// main dashboard window. Tray icon, balloon notifications, host/message lists,
// file download, conversation log and settings live here.

#include "gui_ipmsg.h"
#include "gui_theme.h"
#include "ipc_client.h"
#include "nlohmann/json.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace pc {

namespace {

// ---------------------------------------------------------------------------
// Localization
// ---------------------------------------------------------------------------

enum class UiLang { Ja, En, Es };
UiLang g_lang = UiLang::En;

const wchar_t* T(const wchar_t* ja, const wchar_t* en, const wchar_t* es = nullptr) {
    if (g_lang == UiLang::Ja) return ja;
    if (g_lang == UiLang::Es) return es ? es : en;
    return en;
}

std::string ReadConfigText() {
    return ReadFileUtf8Wide(PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json"));
}

nlohmann::json ChatBlock(const nlohmann::json& cfg) {
    if (cfg.is_object() && cfg.contains("chat") && cfg["chat"].is_object()) return cfg["chat"];
    if (cfg.is_object() && cfg.contains("ipmsg") && cfg["ipmsg"].is_object()) return cfg["ipmsg"];
    return nlohmann::json::object();
}

UiLang DetectLanguage() {
    std::string code;
    std::string content = ReadConfigText();
    if (!content.empty()) {
        nlohmann::json j = nlohmann::json::parse(content, nullptr, false);
        if (j.is_object()) code = j.value("language", std::string());
    }
    if (code.empty()) {
        wchar_t buf[16] = {};
        if (GetEnvironmentVariableW(L"PC_UI_LANG", buf, 16) > 0) {
            if (_wcsnicmp(buf, L"ja", 2) == 0) return UiLang::Ja;
            if (_wcsnicmp(buf, L"es", 2) == 0) return UiLang::Es;
            return UiLang::En;
        }
        switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
            case LANG_JAPANESE: return UiLang::Ja;
            case LANG_SPANISH: return UiLang::Es;
            default: return UiLang::En;
        }
    }
    if (code.rfind("ja", 0) == 0) return UiLang::Ja;
    if (code.rfind("es", 0) == 0) return UiLang::Es;
    return UiLang::En;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

bool IpcCall(const nlohmann::json& req, nlohmann::json& out) {
    std::string resp;
    std::string err;
    if (!SendIpcRequest(req.dump(), resp, err, false)) return false;
    out = nlohmann::json::parse(resp, nullptr, false);
    return !out.is_discarded() && out.is_object();
}

std::string JsonError(const nlohmann::json& j, const std::string& fallback) {
    if (j.is_object() && j.contains("error") && j["error"].is_string()) {
        return j["error"].get<std::string>();
    }
    return fallback;
}

std::wstring GetTextW(HWND h) {
    if (!h) return std::wstring();
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return std::wstring();
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    int got = GetWindowTextW(h, s.data(), n + 1);
    s.resize(got > 0 ? static_cast<size_t>(got) : 0);
    return s;
}

std::string GetTextUtf8(HWND h) {
    return WideToUtf8(GetTextW(h));
}

void SetTextUtf8(HWND h, const std::string& text) {
    if (h) SetWindowTextW(h, Utf8ToWide(text).c_str());
}

std::wstring TruncateW(std::wstring s, size_t maxChars) {
    if (s.size() > maxChars) {
        s.resize(maxChars);
        s += L"...";
    }
    return s;
}

std::wstring PreviewOf(const std::string& text, size_t maxChars = 120) {
    std::string line;
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (!Trim(line).empty()) break;
            line.clear();
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') continue;
        line.push_back(c);
    }
    return TruncateW(Utf8ToWide(Trim(line)), maxChars);
}

std::wstring TimeText(const nlohmann::json& m) {
    std::string t = m.value("time", std::string());
    if (t.size() >= 19) return Utf8ToWide(t.substr(11, 8));
    return Utf8ToWide(t);
}

std::wstring PeerText(const nlohmann::json& msgOrPeer) {
    nlohmann::json p = msgOrPeer;
    if (msgOrPeer.contains("peer") && msgOrPeer["peer"].is_object()) p = msgOrPeer["peer"];
    std::string nick = p.value("nickname", std::string());
    std::string key = p.value("key", std::string());
    std::string host = p.value("host", std::string());
    std::wstring out = Utf8ToWide(!nick.empty() ? nick : host);
    if (out.empty()) out = Utf8ToWide(key);
    if (out.empty()) out = L"?";
    return out;
}

std::wstring AgentsText(const nlohmann::json& m) {
    std::string from = m.value("from_agent", std::string());
    std::string to = m.value("to_agent", std::string());
    std::wstring out;
    if (!from.empty()) out += Utf8ToWide(from);
    if (!to.empty()) {
        if (!out.empty()) out += L" -> ";
        out += Utf8ToWide(to);
    }
    return out;
}

std::wstring FilesText(const nlohmann::json& m) {
    if (!m.contains("files") || !m["files"].is_array()) return std::wstring();
    std::wstring out;
    size_t count = 0;
    for (const auto& f : m["files"]) {
        if (!f.is_object()) continue;
        if (count) out += L", ";
        out += Utf8ToWide(f.value("name", std::string("?")));
        ++count;
        if (count >= 4) {
            if (m["files"].size() > count) out += L", ...";
            break;
        }
    }
    return out;
}

std::wstring FormatSize(uint64_t bytes) {
    wchar_t buf[64] = {};
    if (bytes >= 1024ull * 1024 * 1024) {
        swprintf_s(buf, L"%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024) {
        swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        swprintf_s(buf, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Window / control ids
// ---------------------------------------------------------------------------

const wchar_t* kWndClass = L"HanchoIpmsgWnd";
const wchar_t* kPropDataDir = L"HanchoIpmsgDataDir";

constexpr UINT kTrayMessage = WM_APP + 301;
constexpr UINT_PTR TIMER_POLL = 1;
constexpr UINT_PTR TIMER_PEERS = 2;
constexpr UINT_PTR TIMER_RELOAD = 3;
constexpr UINT kTrayId = 1;

enum ControlId : int {
    IDC_TAB = 2100,
    IDC_PEERS_LABEL = 2101,
    IDC_PEERS = 2102,
    IDC_BTN_REFRESH_PEERS = 2103,
    IDC_PEER_COUNT = 2104,
    IDC_MSG_LABEL = 2105,
    IDC_MESSAGES = 2106,
    IDC_DETAIL_LABEL = 2107,
    IDC_MSG_TEXT = 2108,
    IDC_FILES_LABEL = 2109,
    IDC_FILES = 2110,
    IDC_BTN_DOWNLOAD = 2111,
    IDC_BTN_OPEN_FOLDER = 2112,
    IDC_COMPOSE_TO_LABEL = 2113,
    IDC_COMPOSE_TO = 2114,
    IDC_BTN_ATTACH = 2121,
    IDC_BTN_REMOVE_ATTACH = 2122,
    IDC_ATTACH_LIST = 2123,
    IDC_COMPOSE_TEXT = 2124,
    IDC_COMPOSE_HINT = 2125,
    IDC_BTN_SEND = 2126,
    IDC_LOG_SINCE_LABEL = 2130,
    IDC_LOG_SINCE = 2131,
    IDC_LOG_SEARCH_LABEL = 2132,
    IDC_LOG_SEARCH = 2133,
    IDC_LOG_EVENT_LABEL = 2134,
    IDC_LOG_EVENT = 2135,
    IDC_BTN_LOG_REFRESH = 2136,
    IDC_BTN_LOG_FOLDER = 2137,
    IDC_LOG_LIST = 2138,
    IDC_LOG_COUNT = 2139,
    IDC_SET_ENABLED = 2140,
    IDC_SET_PORT_LABEL = 2141,
    IDC_SET_PORT = 2142,
    IDC_SET_NICK_LABEL = 2143,
    IDC_SET_NICK = 2144,
    IDC_SET_GROUP_LABEL = 2145,
    IDC_SET_GROUP = 2146,
    IDC_SET_HOSTS_LABEL = 2147,
    IDC_SET_HOSTS = 2148,
    IDC_SET_HANDLER_ENABLED = 2149,
    IDC_SET_ENGINE_LABEL = 2150,
    IDC_SET_ENGINE = 2151,
    IDC_SET_MODEL_LABEL = 2152,
    IDC_SET_MODEL = 2153,
    IDC_SET_AGENT_LABEL = 2154,
    IDC_SET_AGENT = 2155,
    IDC_SET_AUTO_REPLY = 2156,
    IDC_BTN_APPLY = 2157,
    IDC_SET_STATUS = 2158,
    IDC_SET_WARNING = 2159,
    IDM_TRAY_OPEN = 2160,
    IDM_TRAY_EXIT = 2161,
};

struct DialogState {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND hwndTab = nullptr;

    HWND peersLabel = nullptr;
    HWND peers = nullptr;
    HWND btnRefreshPeers = nullptr;
    HWND peerCount = nullptr;
    HWND msgLabel = nullptr;
    HWND messages = nullptr;
    HWND detailLabel = nullptr;
    HWND msgText = nullptr;
    HWND filesLabel = nullptr;
    HWND files = nullptr;
    HWND btnDownload = nullptr;
    HWND btnOpenFolder = nullptr;
    HWND composeToLabel = nullptr;
    HWND composeTo = nullptr;
    HWND btnAttach = nullptr;
    HWND btnRemoveAttach = nullptr;
    HWND attachList = nullptr;
    HWND composeText = nullptr;
    HWND composeHint = nullptr;
    HWND btnSend = nullptr;

    HWND logSinceLabel = nullptr;
    HWND logSince = nullptr;
    HWND logSearchLabel = nullptr;
    HWND logSearch = nullptr;
    HWND logEventLabel = nullptr;
    HWND logEvent = nullptr;
    HWND btnLogRefresh = nullptr;
    HWND btnLogFolder = nullptr;
    HWND logList = nullptr;
    HWND logCount = nullptr;

    HWND setEnabled = nullptr;
    HWND setPortLabel = nullptr;
    HWND setPort = nullptr;
    HWND setNickLabel = nullptr;
    HWND setNick = nullptr;
    HWND setGroupLabel = nullptr;
    HWND setGroup = nullptr;
    HWND setHostsLabel = nullptr;
    HWND setHosts = nullptr;
    HWND setHandlerEnabled = nullptr;
    HWND setEngineLabel = nullptr;
    HWND setEngine = nullptr;
    HWND setModelLabel = nullptr;
    HWND setModel = nullptr;
    HWND setAgentLabel = nullptr;
    HWND setAgent = nullptr;
    HWND setAutoReply = nullptr;
    HWND btnApply = nullptr;
    HWND setStatus = nullptr;
    HWND setWarning = nullptr;

    std::vector<HWND> pageChat;
    std::vector<HWND> pageLog;
    std::vector<HWND> pageSettings;
    int currentPage = 0;

    std::vector<nlohmann::json> peersJson;
    std::vector<nlohmann::json> messagesJson;
    nlohmann::json selectedMessage;
    std::vector<std::string> attachFiles;
    std::set<uint64_t> notified;

    int64_t startedMs = 0;
    bool firstMessageLoad = true;
    uint64_t lastVersion = 0;
    bool haveVersion = false;
    bool running = false;
    int unread = 0;
    std::string nickname;
    std::string downloadDir;
    std::string logDir;
    std::wstring currentTitle;

    bool replyActive = false;
    std::string replyPeerKey;
    std::wstring replyLabel;
    uint64_t replyToId = 0;

    NOTIFYICONDATAW nid = {};
    bool trayAdded = false;
    bool exiting = false;
    bool startHidden = false;
    UINT taskbarCreated = 0;

    UINT dpi = 96;
    HFONT font = nullptr;
    HANDLE mutex = nullptr;
    ATOM dataDirAtom = 0;

    DashboardPalette palette;
    HBRUSH bgBrush = nullptr;
    HBRUSH panelBrush = nullptr;
    bool themeApplied = false;
    int64_t lastThemeCheckMs = 0;
    std::map<HWND, bool> checkState;
};

DialogState* g_state = nullptr;
UINT g_showMessage = 0;

int S(int value) {
    const UINT dpi = g_state ? g_state->dpi : 96;
    return MulDiv(value, static_cast<int>(dpi), 96);
}

bool IsCheckboxControl(const DialogState* st, HWND h) {
    if (!st || !h) return false;
    return h == st->setEnabled || h == st->setHandlerEnabled || h == st->setAutoReply;
}

// Applies the dashboard palette to the dialog, its lists and its controls.
// The dialog is a separate process, so the theme is read from config.json.
void ApplyDialogTheme(DialogState* st, bool force = false) {
    if (!st || !st->hwnd) return;
    const DashboardPalette p = GetDashboardPalette();
    if (!force && st->themeApplied && p == st->palette) return;
    st->palette = p;
    st->themeApplied = true;
    if (st->bgBrush) DeleteObject(st->bgBrush);
    if (st->panelBrush) DeleteObject(st->panelBrush);
    st->bgBrush = CreateSolidBrush(p.windowBg);
    st->panelBrush = CreateSolidBrush(p.panel);
    auto styleList = [&p](HWND list) {
        if (!list) return;
        ListView_SetBkColor(list, p.panel);
        ListView_SetTextBkColor(list, p.panel);
        ListView_SetTextColor(list, p.listText);
        InvalidateRect(list, nullptr, TRUE);
    };
    styleList(st->peers);
    styleList(st->messages);
    styleList(st->files);
    styleList(st->logList);
    ApplyDashboardNativeTheme(st->hwnd);
    if (p.dark) {
        auto themeList = [](HWND list) {
            if (!list) return;
            SetWindowTheme(list, L"DarkMode_ItemsView", nullptr);
            if (HWND header = ListView_GetHeader(list)) {
                SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
            }
        };
        themeList(st->peers);
        themeList(st->messages);
        themeList(st->files);
        themeList(st->logList);
        // Buttons and the tab strip need an explicit dark-mode theme + change
        // notification; otherwise they keep the light system colours.
        for (HWND child = GetWindow(st->hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t cls[64] = {};
            GetClassNameW(child, cls, 64);
            if (_wcsicmp(cls, L"Button") == 0 || _wcsicmp(cls, L"SysTabControl32") == 0) {
                SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
                SendMessageW(child, WM_THEMECHANGED, 0, 0);
            }
        }
    } else {
        auto clearList = [](HWND list) {
            if (!list) return;
            SetWindowTheme(list, L"", L"");
            if (HWND header = ListView_GetHeader(list)) SetWindowTheme(header, L"", L"");
        };
        clearList(st->peers);
        clearList(st->messages);
        clearList(st->files);
        clearList(st->logList);
        for (HWND child = GetWindow(st->hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t cls[64] = {};
            GetClassNameW(child, cls, 64);
            if (_wcsicmp(cls, L"Button") == 0 || _wcsicmp(cls, L"SysTabControl32") == 0) {
                SetWindowTheme(child, L"", L"");
                SendMessageW(child, WM_THEMECHANGED, 0, 0);
            }
        }
    }
    InvalidateRect(st->hwnd, nullptr, TRUE);
}

void DestroyDialogTheme(DialogState* st) {
    if (!st) return;
    if (st->bgBrush) DeleteObject(st->bgBrush);
    if (st->panelBrush) DeleteObject(st->panelBrush);
    st->bgBrush = nullptr;
    st->panelBrush = nullptr;
}

// ---------------------------------------------------------------------------
// Control creation helpers
// ---------------------------------------------------------------------------

HWND MakeStatic(HWND parent, int id, const wchar_t* text, DWORD style = SS_LEFT) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | style, 0, 0, 10, 10,
                           parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

HWND MakeButton(HWND parent, int id, const wchar_t* text) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                           0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

HWND MakeCheck(HWND parent, int id, const wchar_t* text) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                           0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

HWND MakeEdit(HWND parent, int id, DWORD style, DWORD exStyle = WS_EX_CLIENTEDGE) {
    return CreateWindowExW(exStyle, L"EDIT", L"", WS_CHILD | WS_VISIBLE | style,
                           0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

HWND MakeComboEdit(HWND parent, int id) {
    return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                           CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

HWND MakeCombo(HWND parent, int id) {
    return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                           CBS_DROPDOWNLIST, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

HWND MakeList(HWND parent, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                           WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                           0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

void AddColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<wchar_t*>(title);
    col.cx = width;
    col.iSubItem = index;
    ListView_InsertColumn(list, index, &col);
}

int InsertRow(HWND list, const std::vector<std::wstring>& cells, LPARAM data) {
    if (!list) return -1;
    LVITEMW item = {};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = ListView_GetItemCount(list);
    item.lParam = data;
    item.pszText = const_cast<wchar_t*>(cells.empty() ? L"" : cells[0].c_str());
    int row = static_cast<int>(SendMessageW(list, LVM_INSERTITEMW, 0,
                                            reinterpret_cast<LPARAM>(&item)));
    if (row < 0) return row;
    for (size_t c = 1; c < cells.size(); ++c) {
        ListView_SetItemText(list, row, static_cast<int>(c),
                             const_cast<wchar_t*>(cells[c].c_str()));
    }
    return row;
}

int SelectedIndex(HWND list) {
    if (!list) return -1;
    int item = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (item < 0) return -1;
    LVITEMW info = {};
    info.mask = LVIF_PARAM;
    info.iItem = item;
    if (!ListView_GetItem(list, &info)) return -1;
    return static_cast<int>(info.lParam);
}

void SelectRowByData(HWND list, LPARAM data) {
    if (!list) return;
    int count = ListView_GetItemCount(list);
    for (int i = 0; i < count; ++i) {
        LVITEMW info = {};
        info.mask = LVIF_PARAM;
        info.iItem = i;
        if (ListView_GetItem(list, &info) && info.lParam == data) {
            ListView_SetItemState(list, i, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list, i, FALSE);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

RECT PageRect(const DialogState* st) {
    RECT rc = {};
    if (!st->hwnd) return rc;
    GetClientRect(st->hwnd, &rc);
    int margin = S(10);
    int tabHeader = S(26);
    RECT page = {margin + S(6), margin + tabHeader + S(4), rc.right - margin - S(6),
                 rc.bottom - margin - S(6)};
    if (page.right < page.left) page.right = page.left;
    if (page.bottom < page.top) page.bottom = page.top;
    return page;
}

void Place(HWND h, int x, int y, int w, int hgt) {
    if (!h || w <= 0 || hgt <= 0) return;
    MoveWindow(h, x, y, w, hgt, TRUE);
}

// The received body and attachments are shown only while composing a reply.
void UpdateReplyDetailVisibility(DialogState* st) {
    if (!st) return;
    const bool show = st->replyActive && st->currentPage == 0;
    HWND controls[] = {st->detailLabel, st->msgText, st->filesLabel, st->files,
                       st->btnDownload, st->btnOpenFolder};
    for (HWND h : controls) {
        if (h) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
    }
}

void LayoutChat(DialogState* st, const RECT& pr) {
    const int w = pr.right - pr.left;
    const int hgt = pr.bottom - pr.top;
    if (w < 60 || hgt < 60) return;

    const int composeH = S(172);
    const int rightW = (std::min)(S(460), w / 2);
    const int rx = pr.right - rightW;
    const int lw = rx - pr.left - S(8);
    if (lw < S(120)) return;
    const int hostBottom = pr.bottom - composeH - S(8);

    // Right upper: hosts.
    Place(st->peersLabel, rx, pr.top, rightW - S(80), S(20));
    Place(st->btnRefreshPeers, rx + rightW - S(76), pr.top, S(76), S(22));
    Place(st->peerCount, rx, pr.top + S(24), rightW, S(18));
    Place(st->peers, rx, pr.top + S(44), rightW,
          (std::max)(S(40), hostBottom - static_cast<int>(pr.top) - S(44)));

    // Left: the message list, plus the received body and attachments while replying.
    const int msgTop = pr.top;
    Place(st->msgLabel, pr.left, msgTop, lw, S(20));
    if (st->replyActive) {
        int msgH = (pr.bottom - msgTop - S(20) - S(8)) * 45 / 100;
        if (msgH < S(60)) msgH = S(60);
        Place(st->messages, pr.left, msgTop + S(20), lw, msgH);

        const int dy = msgTop + S(20) + msgH + S(8);
        const int dh = pr.bottom - dy;
        Place(st->detailLabel, pr.left, dy, lw, S(18));
        int textH = (dh - S(18)) * 55 / 100;
        if (textH < S(40)) textH = S(40);
        Place(st->msgText, pr.left, dy + S(18), lw, textH);

        const int fy = dy + S(18) + textH + S(4);
        Place(st->filesLabel, pr.left, fy, S(200), S(18));
        Place(st->btnDownload, pr.left + lw - S(186), fy, S(88), S(20));
        Place(st->btnOpenFolder, pr.left + lw - S(94), fy, S(94), S(20));
        int filesH = pr.bottom - (fy + S(24));
        if (filesH < S(30)) filesH = S(30);
        Place(st->files, pr.left, fy + S(24), lw, filesH);
    } else {
        int msgH = pr.bottom - (msgTop + S(20));
        if (msgH < S(60)) msgH = S(60);
        Place(st->messages, pr.left, msgTop + S(20), lw, msgH);
    }

    // Right lower: compose.
    const int cy = pr.bottom - composeH;
    int x = rx;
    int y = cy;
    const int labelH = S(22);
    Place(st->composeToLabel, x, y + S(3), S(48), labelH);
    x += S(50);
    Place(st->composeTo, x, y, (std::max)(S(160), rightW - S(50)), S(24));

    const int row2 = y + S(30);
    int attachW = S(190);
    int textW = rightW - attachW - S(8);
    if (textW < S(120)) {
        textW = S(120);
        attachW = (std::max)(S(90), rightW - textW - S(8));
    }
    const int attachX = rx + textW + S(8);
    Place(st->composeText, rx, row2, textW, S(84));
    const int abtnW = (std::max)(S(48), (attachW - S(6)) / 2);
    Place(st->btnAttach, attachX, row2, abtnW, S(22));
    Place(st->btnRemoveAttach, attachX + abtnW + S(6), row2, abtnW, S(22));
    Place(st->attachList, attachX, row2 + S(26), attachW, S(84) - S(26));

    const int row3 = row2 + S(88);
    Place(st->composeHint, rx, row3 + S(5), (std::max)(S(80), rightW - S(120)), S(20));
    Place(st->btnSend, rx + rightW - S(96), row3, S(96), S(28));
}

void LayoutLog(DialogState* st, const RECT& pr) {
    const int w = pr.right - pr.left;
    if (w < 60) return;
    int y = pr.top;
    int x = pr.left;
    Place(st->logSinceLabel, x, y + S(4), S(58), S(20));
    x += S(60);
    Place(st->logSince, x, y, S(110), S(24));
    x += S(110) + S(10);
    Place(st->logSearchLabel, x, y + S(4), S(46), S(20));
    x += S(48);
    int searchW = (std::max)(S(120), w - static_cast<int>(x - pr.left) - S(400));
    Place(st->logSearch, x, y, searchW, S(24));
    x += searchW + S(10);
    Place(st->logEventLabel, x, y + S(4), S(42), S(20));
    x += S(44);
    Place(st->logEvent, x, y, S(130), S(24));
    Place(st->btnLogRefresh, pr.right - S(210), y, S(96), S(24));
    Place(st->btnLogFolder, pr.right - S(108), y, S(108), S(24));

    const int listTop = y + S(32);
    Place(st->logList, pr.left, listTop, w, pr.bottom - listTop - S(22));
    Place(st->logCount, pr.left, pr.bottom - S(20), w, S(18));
}

void LayoutSettings(DialogState* st, const RECT& pr) {
    const int w = pr.right - pr.left;
    if (w < 60) return;
    const int x = pr.left;
    int y = pr.top;

    Place(st->setEnabled, x, y, S(320), S(24));
    y += S(30);
    Place(st->setPortLabel, x, y + S(3), S(70), S(20));
    Place(st->setPort, x + S(76), y, S(90), S(24));
    y += S(30);
    Place(st->setNickLabel, x, y + S(3), S(70), S(20));
    Place(st->setNick, x + S(76), y, S(300), S(24));
    y += S(30);
    Place(st->setGroupLabel, x, y + S(3), S(70), S(20));
    Place(st->setGroup, x + S(76), y, S(300), S(24));
    y += S(30);
    Place(st->setHostsLabel, x, y + S(3), S(70), S(20));
    Place(st->setHosts, x + S(76), y, (std::max)(S(200), w - S(80)), S(24));
    y += S(40);

    Place(st->setHandlerEnabled, x, y, S(360), S(24));
    y += S(30);
    Place(st->setEngineLabel, x, y + S(3), S(70), S(20));
    Place(st->setEngine, x + S(76), y, S(160), S(24));
    y += S(30);
    Place(st->setModelLabel, x, y + S(3), S(70), S(20));
    Place(st->setModel, x + S(76), y, S(260), S(24));
    y += S(30);
    Place(st->setAgentLabel, x, y + S(3), S(70), S(20));
    Place(st->setAgent, x + S(76), y, S(260), S(24));
    y += S(30);
    Place(st->setAutoReply, x, y, S(300), S(24));
    y += S(40);

    Place(st->btnApply, x, y, S(110), S(28));
    Place(st->setStatus, x + S(120), y + S(5), (std::max)(S(120), w - S(130)), S(20));
    y += S(40);

    int warnTop = y;
    int warnBottom = pr.bottom - S(4);
    if (warnBottom - warnTop < S(36)) warnTop = warnBottom - S(36);
    Place(st->setWarning, x, warnTop, w, warnBottom - warnTop);
}

void LayoutCurrentPage(DialogState* st) {
    if (!st || !st->hwnd) return;
    RECT rc = {};
    GetClientRect(st->hwnd, &rc);
    int margin = S(10);
    if (st->hwndTab) {
        MoveWindow(st->hwndTab, margin, margin,
                   (std::max)(0, static_cast<int>(rc.right) - margin * 2),
                   (std::max)(0, static_cast<int>(rc.bottom) - margin * 2), TRUE);
    }
    RECT pr = PageRect(st);
    if (st->currentPage == 1) LayoutLog(st, pr);
    else if (st->currentPage == 2) LayoutSettings(st, pr);
    else LayoutChat(st, pr);
}

void ShowPage(DialogState* st, int page) {
    if (!st) return;
    st->currentPage = page;
    auto show = [](const std::vector<HWND>& list, bool visible) {
        for (HWND h : list) {
            if (h) ShowWindow(h, visible ? SW_SHOW : SW_HIDE);
        }
    };
    show(st->pageChat, page == 0);
    show(st->pageLog, page == 1);
    show(st->pageSettings, page == 2);
    if (page == 0) UpdateReplyDetailVisibility(st);
    LayoutCurrentPage(st);
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------

void UpdateTitleAndTray(DialogState* st);

void AddTrayIcon(DialogState* st) {
    if (!st || st->trayAdded) return;
    st->nid = {};
    st->nid.cbSize = sizeof(NOTIFYICONDATAW);
    st->nid.hWnd = st->hwnd;
    st->nid.uID = kTrayId;
    st->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    st->nid.uCallbackMessage = kTrayMessage;
    st->nid.hIcon = LoadIconW(st->inst, MAKEINTRESOURCEW(2));
    if (!st->nid.hIcon) st->nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(st->nid.szTip, L"Hancho Chat", _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &st->nid)) st->trayAdded = true;
}

void RemoveTrayIcon(DialogState* st) {
    if (!st || !st->trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &st->nid);
    st->trayAdded = false;
}

void UpdateTitleAndTray(DialogState* st) {
    if (!st || !st->hwnd) return;
    std::wstring name = Utf8ToWide(st->nickname);
    if (name.empty()) name = L"Hancho";
    std::wstring title = L"Hancho Chat";
    if (st->unread > 0) {
        title += L" [";
        title += std::to_wstring(st->unread);
        title += L"]";
    }
    title += L" - ";
    title += name;
    if (!st->running) title += T(L" (無効)", L" (disabled)", L" (desactivado)");
    if (title != st->currentTitle) {
        st->currentTitle = title;
        SetWindowTextW(st->hwnd, title.c_str());
    }
    if (st->trayAdded) {
        std::wstring tip = title;
        if (st->unread > 0) {
            tip += L" - ";
            tip += std::to_wstring(st->unread);
            tip += T(L" 件の未読", L" unread", L" sin leer");
        }
        wcsncpy_s(st->nid.szTip, tip.c_str(), _TRUNCATE);
        st->nid.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &st->nid);
    }
}

void ShowBalloon(DialogState* st, const std::wstring& peer, const std::wstring& preview) {
    if (!st) return;
    if (!st->trayAdded) AddTrayIcon(st);
    if (!st->trayAdded) return;
    st->nid.uFlags = NIF_INFO | NIF_TIP | NIF_ICON | NIF_MESSAGE;
    st->nid.dwInfoFlags = NIIF_INFO;
    st->nid.uTimeout = 10000;
    wcsncpy_s(st->nid.szInfoTitle, TruncateW(peer, 60).c_str(), _TRUNCATE);
    wcsncpy_s(st->nid.szInfo, TruncateW(preview, 200).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &st->nid);
}

void ShowMainWindow(DialogState* st) {
    if (!st || !st->hwnd) return;
    if (IsIconic(st->hwnd)) ShowWindow(st->hwnd, SW_RESTORE);
    else ShowWindow(st->hwnd, SW_SHOW);
    BringWindowToTop(st->hwnd);
    // A background process cannot normally take the foreground: attach to the
    // current foreground thread for the call, then detach.
    DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD myThread = GetCurrentThreadId();
    bool attached = (fgThread != 0 && fgThread != myThread &&
                     AttachThreadInput(fgThread, myThread, TRUE) != FALSE);
    SetForegroundWindow(st->hwnd);
    BringWindowToTop(st->hwnd);
    SetActiveWindow(st->hwnd);
    if (attached) AttachThreadInput(fgThread, myThread, FALSE);
}

void ShowTrayMenu(DialogState* st) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, T(L"開く", L"Open", L"Abrir"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT,
                T(L"Hancho Chat終了", L"Quit Hancho Chat", L"Salir de Hancho Chat"));
    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(st->hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, st->hwnd, nullptr);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
// Peers / messages
// ---------------------------------------------------------------------------

std::set<std::string> SelectedPeerKeys(DialogState* st) {
    std::set<std::string> keys;
    if (!st || !st->peers) return keys;
    int item = -1;
    while ((item = ListView_GetNextItem(st->peers, item, LVNI_SELECTED)) >= 0) {
        LVITEMW info = {};
        info.mask = LVIF_PARAM;
        info.iItem = item;
        if (!ListView_GetItem(st->peers, &info)) continue;
        int idx = static_cast<int>(info.lParam);
        if (idx < 0 || static_cast<size_t>(idx) >= st->peersJson.size()) continue;
        std::string key = st->peersJson[static_cast<size_t>(idx)].value("key", std::string());
        if (!key.empty()) keys.insert(key);
    }
    return keys;
}

void UpdatePeerCount(DialogState* st) {
    if (!st || !st->peerCount) return;
    wchar_t buf[160] = {};
    swprintf_s(buf, T(L"ホスト: %d  未読: %d", L"Peers: %d  Unread: %d",
                      L"Equipos: %d  Sin leer: %d"),
               static_cast<int>(st->peersJson.size()), st->unread);
    SetWindowTextW(st->peerCount, buf);
}

void LoadPeers(DialogState* st) {
    if (!st) return;
    nlohmann::json resp;
    if (!IpcCall({{"action", "chat_peers"}}, resp)) return;
    if (!resp.contains("peers") || !resp["peers"].is_array()) return;
    std::set<std::string> selected = SelectedPeerKeys(st);
    st->peersJson.clear();
    for (const auto& p : resp["peers"]) {
        if (p.is_object()) st->peersJson.push_back(p);
    }
    if (st->peers) ListView_DeleteAllItems(st->peers);
    for (size_t i = 0; i < st->peersJson.size(); ++i) {
        const auto& p = st->peersJson[i];
        std::vector<std::wstring> cells = {
            Utf8ToWide(p.value("nickname", std::string())),
            Utf8ToWide(p.value("group", std::string())),
            Utf8ToWide(p.value("host", std::string())),
            p.value("hancho", false) ? L"Hancho" : L""
        };
        InsertRow(st->peers, cells, static_cast<LPARAM>(i));
    }
    for (const auto& key : selected) {
        int count = st->peers ? ListView_GetItemCount(st->peers) : 0;
        for (int i = 0; i < count; ++i) {
            LVITEMW info = {};
            info.mask = LVIF_PARAM;
            info.iItem = i;
            if (!ListView_GetItem(st->peers, &info)) continue;
            int idx = static_cast<int>(info.lParam);
            if (idx < 0 || static_cast<size_t>(idx) >= st->peersJson.size()) continue;
            if (st->peersJson[static_cast<size_t>(idx)].value("key", std::string()) == key) {
                ListView_SetItemState(st->peers, i, LVIS_SELECTED, LVIS_SELECTED);
                break;
            }
        }
    }
    UpdatePeerCount(st);
}

int SelectedMessageIndex(DialogState* st) {
    int idx = SelectedIndex(st ? st->messages : nullptr);
    if (idx < 0 || static_cast<size_t>(idx) >= st->messagesJson.size()) return -1;
    return idx;
}

void ClearMessageDetail(DialogState* st) {
    if (!st) return;
    st->selectedMessage = nlohmann::json();
    if (st->msgText) SetWindowTextW(st->msgText, L"");
    if (st->files) ListView_DeleteAllItems(st->files);
}

void PopulateFiles(DialogState* st, const nlohmann::json& message) {
    if (!st || !st->files) return;
    ListView_DeleteAllItems(st->files);
    if (!message.contains("files") || !message["files"].is_array()) return;
    int i = 0;
    for (const auto& f : message["files"]) {
        if (!f.is_object()) continue;
        uint64_t size = f.value("size", 0ULL);
        uint64_t bytes = f.value("bytes", 0ULL);
        std::wstring progress;
        if (bytes && size) {
            progress = FormatSize(bytes) + L" / " + FormatSize(size);
        } else if (f.contains("error") && f["error"].is_string()) {
            progress = Utf8ToWide(f["error"].get<std::string>());
        }
        std::vector<std::wstring> cells = {
            Utf8ToWide(f.value("name", std::string())),
            FormatSize(size),
            Utf8ToWide(f.value("status", std::string())),
            progress,
            Utf8ToWide(f.value("path", std::string()))
        };
        InsertRow(st->files, cells, static_cast<LPARAM>(i));
        ++i;
    }
}

void SetMessageTimeCell(DialogState* st, int row, const nlohmann::json& msg) {
    if (!st || !st->messages || row < 0) return;
    std::wstring time = TimeText(msg);
    if (!msg.value("read", false) && msg.value("direction", std::string()) != "out") {
        time = L"● " + time;
    }
    ListView_SetItemText(st->messages, row, 0, const_cast<wchar_t*>(time.c_str()));
}

void SelectMessage(DialogState* st, int idx) {
    if (!st || idx < 0 || static_cast<size_t>(idx) >= st->messagesJson.size()) {
        ClearMessageDetail(st);
        return;
    }
    nlohmann::json& msg = st->messagesJson[static_cast<size_t>(idx)];
    st->selectedMessage = msg;
    SetWindowTextW(st->msgText,
                   NormalizeCrLfWide(msg.value("text", std::string())).c_str());
    PopulateFiles(st, msg);
    if (msg.value("direction", std::string()) != "out" && !msg.value("read", false)) {
        uint64_t id = msg.value("id", 0ULL);
        nlohmann::json resp;
        IpcCall({{"action", "chat_read"}, {"ids", nlohmann::json::array({id})}}, resp);
        msg["read"] = true;
        st->selectedMessage = msg;
        int row = ListView_GetNextItem(st->messages, -1, LVNI_SELECTED);
        SetMessageTimeCell(st, row, msg);
        InvalidateRect(st->messages, nullptr, FALSE);
    }
}

void LoadMessages(DialogState* st) {
    if (!st) return;
    nlohmann::json req = {
        {"action", "chat_inbox"},
        {"since_id", 0},
        {"include_outgoing", true},
        {"include_unaddressed", true},
        {"limit", 500}
    };
    nlohmann::json resp;
    if (!IpcCall(req, resp)) return;
    if (!resp.contains("messages") || !resp["messages"].is_array()) return;

    uint64_t selectedId = 0;
    int oldSel = SelectedMessageIndex(st);
    if (oldSel >= 0) selectedId = st->messagesJson[static_cast<size_t>(oldSel)].value("id", 0ULL);

    st->messagesJson.clear();
    for (const auto& m : resp["messages"]) {
        if (m.is_object()) st->messagesJson.push_back(m);
    }
    if (st->messages) ListView_DeleteAllItems(st->messages);
    for (size_t i = 0; i < st->messagesJson.size(); ++i) {
        const auto& m = st->messagesJson[i];
        std::wstring time = TimeText(m);
        if (!m.value("read", false) && m.value("direction", std::string()) != "out") {
            time = L"● " + time;
        }
        std::vector<std::wstring> cells = {
            time,
            Utf8ToWide(m.value("direction", std::string())),
            PeerText(m),
            AgentsText(m),
            PreviewOf(m.value("text", std::string())),
            FilesText(m),
            Utf8ToWide(m.value("status", std::string()))
        };
        InsertRow(st->messages, cells, static_cast<LPARAM>(i));
    }

    // Balloons only for messages that arrived while this dialog was running.
    for (const auto& m : st->messagesJson) {
        uint64_t id = m.value("id", 0ULL);
        const bool first = st->firstMessageLoad;
        bool newlyNotified = st->notified.insert(id).second;
        if (!newlyNotified) continue;
        if (first && m.value("time_ms", 0LL) < st->startedMs) continue;
        if (m.value("direction", std::string()) != "in") continue;
        if (m.value("read", false)) continue;
        if (m.value("audience", std::string()) == "agent") continue;
        std::wstring peer = PeerText(m);
        std::wstring preview = PreviewOf(m.value("text", std::string()), 160);
        if (preview.empty()) preview = FilesText(m);
        ShowBalloon(st, peer, preview);
    }
    st->firstMessageLoad = false;

    if (selectedId) {
        for (size_t i = 0; i < st->messagesJson.size(); ++i) {
            if (st->messagesJson[i].value("id", 0ULL) == selectedId) {
                SelectRowByData(st->messages, static_cast<LPARAM>(i));
                SelectMessage(st, static_cast<int>(i));
                break;
            }
        }
    } else {
        ClearMessageDetail(st);
    }
}

void PollStatus(DialogState* st) {
    if (!st) return;
    nlohmann::json status;
    if (!IpcCall({{"action", "chat_status"}}, status)) return;
    st->running = status.value("running", false);
    st->unread = static_cast<int>(status.value("unread", 0ULL));
    st->nickname = status.value("nickname", std::string());
    st->downloadDir = status.value("download_dir", std::string());
    uint64_t version = status.value("version", 0ULL);
    UpdateTitleAndTray(st);
    UpdatePeerCount(st);
    if (!st->haveVersion || version != st->lastVersion) {
        st->haveVersion = true;
        st->lastVersion = version;
        LoadPeers(st);
        LoadMessages(st);
    }
    if (st->composeHint) {
        if (!st->running) {
            SetWindowTextW(st->composeHint,
                           T(L"チャットは無効です（設定タブで有効にしてください）",
                             L"Chat is disabled (enable it in Settings)",
                             L"El chat está desactivado (actívalo en Ajustes)"));
        } else {
            SetWindowTextW(st->composeHint,
                           T(L"Ctrl+Enter で送信 / 宛先は選択中のホスト、未選択ならローカルエージェント",
                             L"Ctrl+Enter sends / To = selected hosts, or local agents when none",
                             L"Ctrl+Enter envía / Para = equipos seleccionados o agentes locales"));
        }
    }
}

// ---------------------------------------------------------------------------
// Compose
// ---------------------------------------------------------------------------

void RefreshAttachList(DialogState* st) {
    if (!st || !st->attachList) return;
    SendMessageW(st->attachList, LB_RESETCONTENT, 0, 0);
    for (const auto& path : st->attachFiles) {
        std::wstring w = Utf8ToWide(path);
        size_t slash = w.find_last_of(L"\\/");
        std::wstring name = (slash == std::wstring::npos) ? w : w.substr(slash + 1);
        SendMessageW(st->attachList, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name.c_str()));
    }
}

void DoAttach(DialogState* st) {
    if (!st) return;
    std::vector<wchar_t> buffer(65536, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = L"All files\0*.*\0\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    wchar_t* p = buffer.data();
    std::wstring first = p;
    p += first.size() + 1;
    if (*p == L'\0') {
        st->attachFiles.push_back(WideToUtf8(first));
    } else {
        while (*p) {
            std::wstring name = p;
            p += name.size() + 1;
            st->attachFiles.push_back(WideToUtf8(first + L"\\" + name));
        }
    }
    RefreshAttachList(st);
}

void RemoveSelectedAttachment(DialogState* st) {
    if (!st || !st->attachList) return;
    int sel = static_cast<int>(SendMessageW(st->attachList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || static_cast<size_t>(sel) >= st->attachFiles.size()) return;
    st->attachFiles.erase(st->attachFiles.begin() + sel);
    RefreshAttachList(st);
}

void ResetReply(DialogState* st) {
    if (!st) return;
    st->replyActive = false;
    st->replyPeerKey.clear();
    st->replyLabel.clear();
    st->replyToId = 0;
    if (st->composeTo) {
        ComboBox_DeleteString(st->composeTo, 2);
        ComboBox_InsertString(st->composeTo, 2, T(L"返信", L"Reply", L"Responder"));
        ComboBox_SetCurSel(st->composeTo, 0);
        EnableWindow(st->composeTo, TRUE);
    }
    UpdateReplyDetailVisibility(st);
}

void DoSend(DialogState* st) {
    if (!st) return;
    std::string text = GetTextUtf8(st->composeText);
    if (Trim(text).empty() && st->attachFiles.empty()) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    // The sender identity is the agent name configured in Settings (chat.handler.agent).
    std::string fromAgent;
    {
        nlohmann::json cfg = nlohmann::json::parse(ReadConfigText(), nullptr, false);
        nlohmann::json chat = ChatBlock(cfg);
        if (chat.contains("handler") && chat["handler"].is_object()) {
            fromAgent = Trim(chat["handler"].value("agent", std::string()));
        }
        if (fromAgent.empty()) {
            std::string nick = Trim(chat.value("nickname", std::string()));
            if (nick.empty()) nick = "noname";
            fromAgent = nick + "-llm";
        }
    }
    int target = st->composeTo ? ComboBox_GetCurSel(st->composeTo) : 0;
    if (st->replyActive && !st->replyPeerKey.empty()) target = 2;
    std::vector<std::string> targets;
    if (target == 1) {
        targets.push_back("local");
    } else if (target == 2 && !st->replyPeerKey.empty()) {
        targets.push_back(st->replyPeerKey);
    } else {
        std::set<std::string> keys = SelectedPeerKeys(st);
        targets.assign(keys.begin(), keys.end());
        if (targets.empty()) targets.push_back("local");
    }

    bool allOk = true;
    std::string firstError;
    for (const auto& to : targets) {
        nlohmann::json req = {
            {"action", "chat_send"},
            {"to", to},
            {"text", text},
            {"from_agent", fromAgent}
        };
        if (!st->attachFiles.empty()) req["files"] = st->attachFiles;
        if (target == 2 && st->replyToId) req["reply_to"] = st->replyToId;
        nlohmann::json resp;
        if (!IpcCall(req, resp) || !resp.value("ok", false)) {
            allOk = false;
            if (firstError.empty()) firstError = JsonError(resp, "IPC request failed");
        }
    }
    if (!allOk) {
        MessageBoxW(st->hwnd, Utf8ToWide(firstError).c_str(),
                    T(L"送信できませんでした", L"Send failed", L"Error al enviar"),
                    MB_ICONERROR | MB_OK);
        return;
    }
    SetWindowTextW(st->composeText, L"");
    st->attachFiles.clear();
    RefreshAttachList(st);
    ResetReply(st);
    SetTimer(st->hwnd, TIMER_RELOAD, 400, nullptr);
}

// Reply mode: the message list picks the counterpart, so the destination cannot be
// changed here. The lower pane stays in reply mode until New is pressed or a send succeeds.
void EnterReplyMode(DialogState* st, int idx) {
    if (!st || idx < 0 || static_cast<size_t>(idx) >= st->messagesJson.size()) return;
    const nlohmann::json& m = st->messagesJson[static_cast<size_t>(idx)];
    if (m.value("direction", std::string()) == "out") return;
    std::string peerKey;
    if (m.contains("peer") && m["peer"].is_object()) {
        peerKey = m["peer"].value("key", std::string());
    }
    if (peerKey.empty()) peerKey = "local";
    st->replyActive = true;
    st->replyPeerKey = peerKey;
    st->replyToId = m.value("id", 0ULL);
    st->replyLabel = PeerText(m);
    if (st->composeTo) {
        std::wstring label = T(L"返信: ", L"Reply to: ", L"Responder a: ");
        label += st->replyLabel;
        ComboBox_DeleteString(st->composeTo, 2);
        ComboBox_InsertString(st->composeTo, 2, label.c_str());
        ComboBox_SetCurSel(st->composeTo, 2);
    }
    UpdateReplyDetailVisibility(st);
}

// Reply mode without a message (nothing selected): show empty body/attachments.
void EnterEmptyReplyMode(DialogState* st) {
    if (!st) return;
    st->replyActive = true;
    st->replyPeerKey.clear();
    st->replyToId = 0;
    st->replyLabel.clear();
    if (st->msgText) SetWindowTextW(st->msgText, L"");
    if (st->files) ListView_DeleteAllItems(st->files);
    if (st->composeTo) {
        ComboBox_DeleteString(st->composeTo, 2);
        ComboBox_InsertString(st->composeTo, 2, T(L"返信", L"Reply", L"Responder"));
        ComboBox_SetCurSel(st->composeTo, 2);
    }
    UpdateReplyDetailVisibility(st);
}

// ---------------------------------------------------------------------------
// Attachments
// ---------------------------------------------------------------------------

int SelectedFileIndex(DialogState* st) {
    return SelectedIndex(st ? st->files : nullptr);
}

void DoDownload(DialogState* st) {
    if (!st) return;
    int idx = SelectedMessageIndex(st);
    if (idx < 0) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    const nlohmann::json& m = st->messagesJson[static_cast<size_t>(idx)];
    uint64_t id = m.value("id", 0ULL);
    int64_t fileId = -1;
    int fi = SelectedFileIndex(st);
    if (fi >= 0 && m.contains("files") && m["files"].is_array() &&
        static_cast<size_t>(fi) < m["files"].size()) {
        fileId = m["files"][static_cast<size_t>(fi)].value("file_id", static_cast<int64_t>(-1));
    }
    nlohmann::json resp;
    nlohmann::json req = {
        {"action", "chat_download"},
        {"message_id", id},
        {"file_id", fileId},
        {"wait", false}
    };
    if (!IpcCall(req, resp) || !resp.value("ok", false)) {
        std::string err = JsonError(resp, "IPC request failed");
        MessageBoxW(st->hwnd, Utf8ToWide(err).c_str(),
                    T(L"ダウンロードできません", L"Download failed", L"Error al descargar"),
                    MB_ICONERROR | MB_OK);
        return;
    }
    SetTimer(st->hwnd, TIMER_RELOAD, 900, nullptr);
}

void OpenFolder(const std::wstring& path) {
    if (path.empty()) return;
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void DoOpenDownloadFolder(DialogState* st) {
    if (!st) return;
    std::wstring target;
    int fi = SelectedFileIndex(st);
    if (fi >= 0 && st->selectedMessage.contains("files") &&
        st->selectedMessage["files"].is_array() &&
        static_cast<size_t>(fi) < st->selectedMessage["files"].size()) {
        std::string p = st->selectedMessage["files"][static_cast<size_t>(fi)]
                            .value("path", std::string());
        if (!p.empty()) {
            std::wstring w = Utf8ToWide(p);
            size_t slash = w.find_last_of(L"\\/");
            if (slash != std::wstring::npos) target = w.substr(0, slash);
        }
    }
    if (target.empty() && !st->downloadDir.empty()) target = Utf8ToWide(st->downloadDir);
    if (target.empty()) target = PathJoinWide(GetEffectiveDataDirectoryWide(), L"chat\\files");
    OpenFolder(target);
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

void LoadLog(DialogState* st) {
    if (!st) return;
    std::string since = "7d";
    int sinceIdx = st->logSince ? ComboBox_GetCurSel(st->logSince) : 1;
    if (sinceIdx == 0) since = "today";
    else if (sinceIdx == 2) since = "30d";
    std::string query = GetTextUtf8(st->logSearch);
    std::string event;
    int eventIdx = st->logEvent ? ComboBox_GetCurSel(st->logEvent) : 0;
    static const char* events[] = {"", "recv", "send", "local", "status", "file_sent",
                                   "file_received"};
    if (eventIdx > 0 && eventIdx < static_cast<int>(sizeof(events) / sizeof(events[0]))) {
        event = events[eventIdx];
    }
    nlohmann::json req = {
        {"action", "chat_log"},
        {"since", since},
        {"query", query},
        {"limit", 1000}
    };
    if (!event.empty()) req["event"] = event;
    nlohmann::json resp;
    if (!IpcCall(req, resp)) return;
    st->logDir = resp.value("log_dir", std::string());
    if (st->logList) ListView_DeleteAllItems(st->logList);
    if (!resp.contains("entries") || !resp["entries"].is_array()) return;
    int count = 0;
    for (const auto& e : resp["entries"]) {
        if (!e.is_object()) continue;
        std::wstring peer;
        if (e.contains("peer")) {
            if (e["peer"].is_object()) peer = PeerText(e["peer"]);
            else if (e["peer"].is_string()) peer = Utf8ToWide(e["peer"].get<std::string>());
        }
        std::wstring detail = Utf8ToWide(e.value("text", std::string()));
        if (detail.empty()) {
            std::string status = e.value("status", std::string());
            std::string path = e.value("path", std::string());
            std::string error = e.value("error", std::string());
            detail = Utf8ToWide(!status.empty() ? status : (!path.empty() ? path : error));
        }
        std::wstring from = Utf8ToWide(e.value("from_agent", std::string()));
        std::wstring to = Utf8ToWide(e.value("to_agent", std::string()));
        std::vector<std::wstring> cells = {
            Utf8ToWide(e.value("logged_at", e.value("time", std::string()))),
            Utf8ToWide(e.value("event", std::string())),
            peer,
            from,
            to,
            TruncateW(detail, 160)
        };
        InsertRow(st->logList, cells, static_cast<LPARAM>(count));
        ++count;
    }
    if (st->logCount) {
        wchar_t buf[160] = {};
        swprintf_s(buf, T(L"%d 件のログ", L"%d log entries", L"%d entradas de registro"),
                   count);
        SetWindowTextW(st->logCount, buf);
    }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void SetCheck(HWND h, bool on) {
    if (!h) return;
    if (g_state) g_state->checkState[h] = on;
    InvalidateRect(h, nullptr, TRUE);
}

bool GetCheck(HWND h) {
    if (!h || !g_state) return false;
    auto it = g_state->checkState.find(h);
    return it != g_state->checkState.end() && it->second;
}

void LoadSettings(DialogState* st) {
    if (!st) return;
    nlohmann::json resp;
    nlohmann::json chat = nlohmann::json::object();
    if (IpcCall({{"action", "config_get"}}, resp) && resp.value("ok", false) &&
        resp.contains("config") && resp["config"].is_object()) {
        chat = ChatBlock(resp["config"]);
    }
    if (chat.empty()) {
        const std::string content = ReadConfigText();
        if (!content.empty()) {
            chat = ChatBlock(nlohmann::json::parse(content, nullptr, false));
        }
    }
    SetCheck(st->setEnabled, chat.value("enabled", false));
    SetTextUtf8(st->setPort, std::to_string(chat.value("port", 2426)));
    SetTextUtf8(st->setNick, chat.value("nickname", std::string()));
    SetTextUtf8(st->setGroup, chat.value("group", std::string("Hancho")));
    std::string hosts;
    if (chat.contains("hosts") && chat["hosts"].is_array()) {
        for (const auto& h : chat["hosts"]) {
            if (!h.is_string()) continue;
            if (!hosts.empty()) hosts += ", ";
            hosts += h.get<std::string>();
        }
    }
    SetTextUtf8(st->setHosts, hosts);

    nlohmann::json handler = nlohmann::json::object();
    if (chat.contains("handler") && chat["handler"].is_object()) handler = chat["handler"];
    SetCheck(st->setHandlerEnabled, handler.value("enabled", false));
    SetTextUtf8(st->setEngine, handler.value("engine", std::string("agy")));
    SetTextUtf8(st->setModel, handler.value("model", std::string()));
    {
        std::string agent = handler.value("agent", std::string());
        if (agent.empty()) {
            std::string nick = chat.value("nickname", std::string());
            if (nick.empty()) nick = "noname";
            agent = nick + "-llm";
        }
        SetTextUtf8(st->setAgent, agent);
    }
    SetCheck(st->setAutoReply, handler.value("auto_reply", true));
}

std::vector<std::string> SplitCsv(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == ',') {
            std::string t = Trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    std::string t = Trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

void DoApplySettings(DialogState* st) {
    if (!st) return;
    std::string portText = Trim(GetTextUtf8(st->setPort));
    int port = std::atoi(portText.c_str());
    if (port <= 0 || port > 65535) {
        MessageBoxW(st->hwnd,
                    T(L"ポートは 1 から 65535 の範囲で指定してください。",
                      L"Port must be between 1 and 65535.",
                      L"El puerto debe estar entre 1 y 65535."),
                    T(L"設定", L"Settings", L"Ajustes"), MB_ICONWARNING | MB_OK);
        return;
    }

    nlohmann::json base = nlohmann::json::object();
    nlohmann::json resp;
    if (IpcCall({{"action", "config_get"}}, resp) && resp.value("ok", false) &&
        resp.contains("config") && resp["config"].is_object()) {
        base = ChatBlock(resp["config"]);
    } else {
        const std::string content = ReadConfigText();
        if (!content.empty()) {
            base = ChatBlock(nlohmann::json::parse(content, nullptr, false));
        }
    }

    nlohmann::json chat = base.is_object() ? base : nlohmann::json::object();
    chat["enabled"] = GetCheck(st->setEnabled);
    chat["port"] = port;
    chat["nickname"] = GetTextUtf8(st->setNick);
    chat["group"] = GetTextUtf8(st->setGroup);
    chat["hosts"] = nlohmann::json::array();
    for (const auto& host : SplitCsv(GetTextUtf8(st->setHosts))) {
        chat["hosts"].push_back(host);
    }
    nlohmann::json handler = nlohmann::json::object();
    if (chat.contains("handler") && chat["handler"].is_object()) handler = chat["handler"];
    handler["enabled"] = GetCheck(st->setHandlerEnabled);
    handler["engine"] = GetTextUtf8(st->setEngine);
    handler["model"] = GetTextUtf8(st->setModel);
    handler["agent"] = GetTextUtf8(st->setAgent);
    handler["auto_reply"] = GetCheck(st->setAutoReply);
    chat["handler"] = handler;

    nlohmann::json req = {
        {"action", "config_set"},
        {"config", {{"chat", chat}}}
    };
    nlohmann::json setResp;
    if (!IpcCall(req, setResp) || !setResp.value("ok", false)) {
        std::string err = JsonError(setResp, "IPC request failed");
        MessageBoxW(st->hwnd, Utf8ToWide(err).c_str(),
                    T(L"設定を保存できません", L"Could not save settings",
                      L"No se pudo guardar la configuración"),
                    MB_ICONERROR | MB_OK);
        return;
    }
    if (st->setStatus) {
        SetWindowTextW(st->setStatus, T(L"適用しました", L"Applied", L"Aplicado"));
    }
    SetTimer(st->hwnd, TIMER_POLL, 300, nullptr);
}


// ---------------------------------------------------------------------------
// Fonts and control creation
// ---------------------------------------------------------------------------

HFONT MakeFont(UINT dpi) {
    return CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE,
                       FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       L"Segoe UI");
}

void ApplyFont(DialogState* st) {
    if (!st || !st->font) return;
    std::vector<HWND> all;
    auto push = [&](HWND h) { if (h) all.push_back(h); };
    push(st->hwndTab);
    push(st->peersLabel); push(st->peers); push(st->btnRefreshPeers); push(st->peerCount);
    push(st->msgLabel); push(st->messages); push(st->detailLabel); push(st->msgText);
    push(st->filesLabel); push(st->files); push(st->btnDownload); push(st->btnOpenFolder);
    push(st->composeToLabel); push(st->composeTo);
    push(st->btnAttach);
    push(st->btnRemoveAttach); push(st->attachList); push(st->composeText);
    push(st->composeHint); push(st->btnSend);
    push(st->logSinceLabel); push(st->logSince); push(st->logSearchLabel);
    push(st->logSearch); push(st->logEventLabel); push(st->logEvent);
    push(st->btnLogRefresh); push(st->btnLogFolder); push(st->logList); push(st->logCount);
    push(st->setEnabled); push(st->setPortLabel); push(st->setPort);
    push(st->setNickLabel); push(st->setNick); push(st->setGroupLabel); push(st->setGroup);
    push(st->setHostsLabel); push(st->setHosts); push(st->setHandlerEnabled);
    push(st->setEngineLabel); push(st->setEngine); push(st->setModelLabel); push(st->setModel);
    push(st->setAgentLabel); push(st->setAgent); push(st->setAutoReply);
    push(st->btnApply); push(st->setStatus); push(st->setWarning);
    for (HWND h : all) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
    }
}

void RecreateFont(DialogState* st) {
    if (!st) return;
    HFONT old = st->font;
    st->font = MakeFont(st->dpi);
    ApplyFont(st);
    if (old) DeleteObject(old);
}

LRESULT CALLBACK ComposeEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR, DWORD_PTR) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (msg == WM_KEYDOWN && wParam == VK_RETURN && ctrl) {
        PostMessageW(GetParent(hwnd), WM_COMMAND,
                     MAKEWPARAM(IDC_BTN_SEND, BN_CLICKED), 0);
        return 0;
    }
    if (msg == WM_CHAR && ctrl && (wParam == L'\r' || wParam == L'\n')) return 0;
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND && g_state && g_state->bgBrush) {
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, g_state->bgBrush);
        return 1;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void CreateChatControls(DialogState* st) {
    HWND hwnd = st->hwnd;
    st->peersLabel = MakeStatic(hwnd, IDC_PEERS_LABEL, T(L"ホスト", L"HOSTS", L"EQUIPOS"));
    st->peers = MakeList(hwnd, IDC_PEERS);
    ListView_SetExtendedListViewStyle(st->peers, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    AddColumn(st->peers, 0, T(L"ニックネーム", L"Nickname", L"Apodo"), S(110));
    AddColumn(st->peers, 1, T(L"グループ", L"Group", L"Grupo"), S(80));
    AddColumn(st->peers, 2, T(L"ホスト", L"Host", L"Host"), S(110));
    AddColumn(st->peers, 3, T(L"マーク", L"Mark", L"Marca"), S(60));
    st->btnRefreshPeers = MakeButton(hwnd, IDC_BTN_REFRESH_PEERS,
                                     T(L"更新", L"Refresh", L"Actualizar"));
    st->peerCount = MakeStatic(hwnd, IDC_PEER_COUNT, L"");

    st->msgLabel = MakeStatic(hwnd, IDC_MSG_LABEL, T(L"メッセージ", L"MESSAGES", L"MENSAJES"));
    st->messages = MakeList(hwnd, IDC_MESSAGES);
    ListView_SetExtendedListViewStyle(st->messages,
                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    AddColumn(st->messages, 0, T(L"時刻", L"Time", L"Hora"), S(72));
    AddColumn(st->messages, 1, T(L"方向", L"Dir", L"Dir"), S(50));
    AddColumn(st->messages, 2, T(L"ピア", L"Peer", L"Par"), S(120));
    AddColumn(st->messages, 3, T(L"エージェント", L"Agents", L"Agentes"), S(110));
    AddColumn(st->messages, 4, T(L"プレビュー", L"Preview", L"Vista previa"), S(240));
    AddColumn(st->messages, 5, T(L"ファイル", L"Files", L"Archivos"), S(120));
    AddColumn(st->messages, 6, T(L"状態", L"Status", L"Estado"), S(90));

    st->detailLabel = MakeStatic(hwnd, IDC_DETAIL_LABEL,
                                 T(L"受信本文", L"RECEIVED MESSAGE", L"MENSAJE RECIBIDO"));
    st->msgText = MakeEdit(hwnd, IDC_MSG_TEXT,
                           ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
    st->filesLabel = MakeStatic(hwnd, IDC_FILES_LABEL,
                                T(L"受信添付ファイル", L"RECEIVED ATTACHMENTS", L"ADJUNTOS RECIBIDOS"));
    st->files = MakeList(hwnd, IDC_FILES);
    ListView_SetExtendedListViewStyle(st->files, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    AddColumn(st->files, 0, T(L"名前", L"Name", L"Nombre"), S(200));
    AddColumn(st->files, 1, T(L"サイズ", L"Size", L"Tamaño"), S(90));
    AddColumn(st->files, 2, T(L"状態", L"Status", L"Estado"), S(90));
    AddColumn(st->files, 3, T(L"進捗", L"Progress", L"Progreso"), S(160));
    AddColumn(st->files, 4, T(L"パス", L"Path", L"Ruta"), S(240));
    st->btnDownload = MakeButton(hwnd, IDC_BTN_DOWNLOAD,
                                 T(L"ダウンロード", L"Download", L"Descargar"));
    st->btnOpenFolder = MakeButton(hwnd, IDC_BTN_OPEN_FOLDER,
                                   T(L"フォルダを開く", L"Open folder", L"Abrir carpeta"));

    st->composeToLabel = MakeStatic(hwnd, IDC_COMPOSE_TO_LABEL,
                                    T(L"送信先", L"Send to", L"Enviar a"));
    st->composeTo = MakeCombo(hwnd, IDC_COMPOSE_TO);
    ComboBox_AddString(st->composeTo,
                       T(L"選択中のホスト", L"Selected host(s)", L"Equipos seleccionados"));
    ComboBox_AddString(st->composeTo,
                       T(L"ローカルエージェント", L"Local agents", L"Agentes locales"));
    ComboBox_AddString(st->composeTo, T(L"返信", L"Reply", L"Responder"));
    ComboBox_SetCurSel(st->composeTo, 0);
    st->btnAttach = MakeButton(hwnd, IDC_BTN_ATTACH,
                               T(L"添付...", L"Attach...", L"Adjuntar..."));
    st->btnRemoveAttach = MakeButton(hwnd, IDC_BTN_REMOVE_ATTACH,
                                     T(L"削除", L"Remove", L"Quitar"));
    st->attachList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                     LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
                                     0, 0, 10, 10, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ATTACH_LIST)),
                                     GetModuleHandleW(nullptr), nullptr);
    st->composeText = MakeEdit(hwnd, IDC_COMPOSE_TEXT,
                               ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
    SetWindowSubclass(st->composeText, ComposeEditProc, 1, 0);
    st->composeHint = MakeStatic(hwnd, IDC_COMPOSE_HINT, L"Ctrl+Enter");
    st->btnSend = MakeButton(hwnd, IDC_BTN_SEND, T(L"送信", L"Send", L"Enviar"));

    st->pageChat = {st->peersLabel, st->peers, st->btnRefreshPeers, st->peerCount,
                    st->msgLabel, st->messages, st->detailLabel, st->msgText,
                    st->filesLabel, st->files, st->btnDownload, st->btnOpenFolder,
                    st->composeToLabel, st->composeTo,
                    st->btnAttach, st->btnRemoveAttach, st->attachList, st->composeText,
                    st->composeHint, st->btnSend};
}

void CreateLogControls(DialogState* st) {
    HWND hwnd = st->hwnd;
    st->logSinceLabel = MakeStatic(hwnd, IDC_LOG_SINCE_LABEL,
                                   T(L"期間", L"Since", L"Desde"));
    st->logSince = MakeCombo(hwnd, IDC_LOG_SINCE);
    ComboBox_AddString(st->logSince, T(L"今日", L"Today", L"Hoy"));
    ComboBox_AddString(st->logSince, T(L"7日", L"7 days", L"7 días"));
    ComboBox_AddString(st->logSince, T(L"30日", L"30 days", L"30 días"));
    ComboBox_SetCurSel(st->logSince, 1);
    st->logSearchLabel = MakeStatic(hwnd, IDC_LOG_SEARCH_LABEL,
                                    T(L"検索", L"Search", L"Buscar"));
    st->logSearch = MakeEdit(hwnd, IDC_LOG_SEARCH, ES_AUTOHSCROLL);
    st->logEventLabel = MakeStatic(hwnd, IDC_LOG_EVENT_LABEL,
                                   T(L"イベント", L"Event", L"Evento"));
    st->logEvent = MakeCombo(hwnd, IDC_LOG_EVENT);
    ComboBox_AddString(st->logEvent, T(L"すべて", L"All", L"Todos"));
    ComboBox_AddString(st->logEvent, L"recv");
    ComboBox_AddString(st->logEvent, L"send");
    ComboBox_AddString(st->logEvent, L"local");
    ComboBox_AddString(st->logEvent, L"status");
    ComboBox_AddString(st->logEvent, L"file_sent");
    ComboBox_AddString(st->logEvent, L"file_received");
    ComboBox_SetCurSel(st->logEvent, 0);
    st->btnLogRefresh = MakeButton(hwnd, IDC_BTN_LOG_REFRESH,
                                   T(L"更新", L"Refresh", L"Actualizar"));
    st->btnLogFolder = MakeButton(hwnd, IDC_BTN_LOG_FOLDER,
                                  T(L"ログを開く", L"Open log folder", L"Abrir carpeta de registro"));
    st->logList = MakeList(hwnd, IDC_LOG_LIST);
    ListView_SetExtendedListViewStyle(st->logList,
                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    AddColumn(st->logList, 0, T(L"時刻", L"Time", L"Hora"), S(150));
    AddColumn(st->logList, 1, T(L"イベント", L"Event", L"Evento"), S(100));
    AddColumn(st->logList, 2, T(L"ピア", L"Peer", L"Par"), S(150));
    AddColumn(st->logList, 3, T(L"送信元", L"From", L"De"), S(120));
    AddColumn(st->logList, 4, T(L"送信先", L"To", L"Para"), S(120));
    AddColumn(st->logList, 5, T(L"本文 / 状態", L"Text / status", L"Texto / estado"), S(380));
    st->logCount = MakeStatic(hwnd, IDC_LOG_COUNT, L"");

    st->pageLog = {st->logSinceLabel, st->logSince, st->logSearchLabel, st->logSearch,
                   st->logEventLabel, st->logEvent, st->btnLogRefresh, st->btnLogFolder,
                   st->logList, st->logCount};
}

void CreateSettingsControls(DialogState* st) {
    HWND hwnd = st->hwnd;
    st->setEnabled = MakeCheck(hwnd, IDC_SET_ENABLED,
                               T(L"チャットを有効にする", L"Enable Chat", L"Activar chat"));
    st->setPortLabel = MakeStatic(hwnd, IDC_SET_PORT_LABEL, T(L"ポート", L"Port", L"Puerto"));
    st->setPort = MakeEdit(hwnd, IDC_SET_PORT, ES_AUTOHSCROLL | ES_NUMBER);
    st->setNickLabel = MakeStatic(hwnd, IDC_SET_NICK_LABEL,
                                  T(L"ニックネーム", L"Nickname", L"Apodo"));
    st->setNick = MakeEdit(hwnd, IDC_SET_NICK, ES_AUTOHSCROLL);
    st->setGroupLabel = MakeStatic(hwnd, IDC_SET_GROUP_LABEL,
                                   T(L"グループ", L"Group", L"Grupo"));
    st->setGroup = MakeEdit(hwnd, IDC_SET_GROUP, ES_AUTOHSCROLL);
    st->setHostsLabel = MakeStatic(hwnd, IDC_SET_HOSTS_LABEL,
                                   T(L"ホスト (csv)", L"Hosts (csv)", L"Equipos (csv)"));
    st->setHosts = MakeEdit(hwnd, IDC_SET_HOSTS, ES_AUTOHSCROLL);
    st->setHandlerEnabled = MakeCheck(hwnd, IDC_SET_HANDLER_ENABLED,
                                      T(L"LLM ハンドラを有効にする",
                                        L"Enable LLM handler", L"Activar manejador LLM"));
    st->setEngineLabel = MakeStatic(hwnd, IDC_SET_ENGINE_LABEL,
                                    T(L"エンジン", L"Engine", L"Motor"));
    st->setEngine = MakeEdit(hwnd, IDC_SET_ENGINE, ES_AUTOHSCROLL);
    st->setModelLabel = MakeStatic(hwnd, IDC_SET_MODEL_LABEL,
                                   T(L"モデル", L"Model", L"Modelo"));
    st->setModel = MakeEdit(hwnd, IDC_SET_MODEL, ES_AUTOHSCROLL);
    st->setAgentLabel = MakeStatic(hwnd, IDC_SET_AGENT_LABEL,
                                   T(L"エージェント", L"Agent name", L"Nombre del agente"));
    st->setAgent = MakeEdit(hwnd, IDC_SET_AGENT, ES_AUTOHSCROLL);
    st->setAutoReply = MakeCheck(hwnd, IDC_SET_AUTO_REPLY,
                                 T(L"自動返信", L"Auto reply", L"Respuesta automática"));
    st->btnApply = MakeButton(hwnd, IDC_BTN_APPLY, T(L"適用", L"Apply", L"Aplicar"));
    st->setStatus = MakeStatic(hwnd, IDC_SET_STATUS, L"");
    st->setWarning = MakeStatic(hwnd, IDC_SET_WARNING,
                                T(L"LAN 内のみ - 暗号化なし。受信メッセージは LLM で処理される場合があります。",
                                  L"LAN only - not encrypted; received messages may be processed by an LLM",
                                  L"Solo LAN: sin cifrado; los mensajes recibidos pueden procesarse con un LLM"));

    st->pageSettings = {st->setEnabled, st->setPortLabel, st->setPort,
                        st->setNickLabel, st->setNick, st->setGroupLabel, st->setGroup,
                        st->setHostsLabel, st->setHosts, st->setHandlerEnabled,
                        st->setEngineLabel, st->setEngine, st->setModelLabel, st->setModel,
                        st->setAgentLabel, st->setAgent, st->setAutoReply,
                        st->btnApply, st->setStatus, st->setWarning};
}

// ---------------------------------------------------------------------------
// Window procedure / entry point
// ---------------------------------------------------------------------------

UINT SystemDpi() {
    HDC dc = GetDC(nullptr);
    UINT dpi = 96;
    if (dc) {
        int x = GetDeviceCaps(dc, LOGPIXELSX);
        if (x > 0) dpi = static_cast<UINT>(x);
        ReleaseDC(nullptr, dc);
    }
    return dpi;
}

LRESULT CALLBACK IpmsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogState* st = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        st = reinterpret_cast<DialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (st && g_showMessage && msg == g_showMessage) {
        ShowMainWindow(st);
        return 0;
    }
    if (st && st->taskbarCreated && msg == st->taskbarCreated) {
        st->trayAdded = false;
        AddTrayIcon(st);
        UpdateTitleAndTray(st);
        return 0;
    }

    switch (msg) {
        case WM_ERASEBKGND: {
            if (!st) break;
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc,
                     st->bgBrush ? st->bgBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            if (!st) break;
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            SetTextColor(dc, IsWindowEnabled(control) ? st->palette.text : st->palette.muted);
            SetBkColor(dc, st->palette.windowBg);
            return reinterpret_cast<LRESULT>(st->bgBrush ? st->bgBrush
                                                         : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            if (!st) break;
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, st->palette.text);
            SetBkColor(dc, st->palette.panel);
            return reinterpret_cast<LRESULT>(st->panelBrush ? st->panelBrush
                                                            : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        }
        case WM_CREATE: {
            if (!st) return -1;
            st->hwnd = hwnd;
            st->inst = GetModuleHandleW(nullptr);
            st->dpi = SystemDpi();
            st->startedMs = static_cast<int64_t>(GetNowTickMs());
            st->taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
            st->font = MakeFont(st->dpi);

            std::wstring dataDir = GetEffectiveDataDirectoryWide();
            st->dataDirAtom = GlobalAddAtomW(dataDir.c_str());
            if (st->dataDirAtom) {
                SetPropW(hwnd, kPropDataDir,
                         reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(st->dataDirAtom)));
            }

            st->hwndTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                          WS_CHILD | WS_VISIBLE | TCS_TABS | TCS_SINGLELINE | TCS_OWNERDRAWFIXED,
                                          0, 0, 10, 10, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TAB)),
                                          st->inst, nullptr);
            if (st->hwndTab) {
                SetWindowTheme(st->hwndTab, L"", L"");
                SetWindowSubclass(st->hwndTab, TabSubclassProc, 1, 0);
                const wchar_t* labels[3] = {
                    T(L"チャット", L"Chat", L"Chat"),
                    T(L"ログ", L"Log", L"Registro"),
                    T(L"設定", L"Settings", L"Ajustes")
                };
                for (int i = 0; i < 3; ++i) {
                    TCITEMW item = {};
                    item.mask = TCIF_TEXT;
                    item.pszText = const_cast<wchar_t*>(labels[i]);
                    TabCtrl_InsertItem(st->hwndTab, i, &item);
                }
            }

            CreateChatControls(st);
            CreateLogControls(st);
            CreateSettingsControls(st);
            ApplyFont(st);
            ApplyDialogTheme(st, true);
            ShowPage(st, 0);
            LoadSettings(st);
            AddTrayIcon(st);
            UpdateTitleAndTray(st);
            PollStatus(st);
            SetTimer(hwnd, TIMER_POLL, 1000, nullptr);
            return 0;
        }
        case WM_SIZE:
            LayoutCurrentPage(st);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = S(780);
            mmi->ptMinTrackSize.y = S(560);
            return 0;
        }
        case WM_DPICHANGED: {
            if (st) {
                UINT dpi = HIWORD(wParam);
                if (dpi) st->dpi = dpi;
                RECT* rc = reinterpret_cast<RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                             rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
                RecreateFont(st);
                LayoutCurrentPage(st);
            }
            return 0;
        }
        case WM_TIMER:
            if (!st) return 0;
            if (wParam == TIMER_POLL) {
                PollStatus(st);
                const int64_t now = static_cast<int64_t>(GetNowTickMs());
                if (st->lastThemeCheckMs == 0 || now - st->lastThemeCheckMs > 5000) {
                    st->lastThemeCheckMs = now;
                    ApplyDialogTheme(st);
                }
            } else if (wParam == TIMER_PEERS) {
                KillTimer(hwnd, TIMER_PEERS);
                LoadPeers(st);
            } else if (wParam == TIMER_RELOAD) {
                KillTimer(hwnd, TIMER_RELOAD);
                PollStatus(st);
            }
            return 0;
        case WM_DRAWITEM: {
            if (!st) break;
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || !dis->hDC) break;
            HDC dc = dis->hDC;
            RECT rc = dis->rcItem;
            const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
            if (dis->CtlType == ODT_TAB) {
                const bool selected = (dis->itemState & ODS_SELECTED) != 0;
                const COLORREF bg = selected ? st->palette.panel : st->palette.windowBg;
                HBRUSH brush = CreateSolidBrush(bg);
                FillRect(dc, &rc, brush);
                DeleteObject(brush);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, selected ? st->palette.text : st->palette.muted);
                HFONT font = reinterpret_cast<HFONT>(SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
                HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
                wchar_t label[128] = {};
                TCITEMW item = {};
                item.mask = TCIF_TEXT;
                item.pszText = label;
                item.cchTextMax = 128;
                TabCtrl_GetItem(dis->hwndItem, dis->itemID, &item);
                DrawTextW(dc, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (oldFont) SelectObject(dc, oldFont);
                return TRUE;
            }
            if (dis->CtlType == ODT_BUTTON && IsCheckboxControl(st, dis->hwndItem)) {
                HBRUSH bgBrush = CreateSolidBrush(st->palette.windowBg);
                FillRect(dc, &rc, bgBrush);
                DeleteObject(bgBrush);
                const int box = S(14);
                RECT mark = {rc.left + S(2), rc.top + (rc.bottom - rc.top - box) / 2,
                             rc.left + S(2) + box, rc.top + (rc.bottom - rc.top - box) / 2 + box};
                const bool checked = GetCheck(dis->hwndItem);
                HBRUSH fill = CreateSolidBrush(checked ? st->palette.accent : st->palette.panel);
                FillRect(dc, &mark, fill);
                DeleteObject(fill);
                HPEN pen = CreatePen(PS_SOLID, 1, st->palette.border);
                HGDIOBJ oldPen = SelectObject(dc, pen);
                HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, mark.left, mark.top, mark.right, mark.bottom);
                if (checked) {
                    HPEN checkPen = CreatePen(PS_SOLID, 2, st->palette.text);
                    HGDIOBJ oldCheck = SelectObject(dc, checkPen);
                    MoveToEx(dc, mark.left + S(3), mark.top + S(7), nullptr);
                    LineTo(dc, mark.left + S(6), mark.bottom - S(3));
                    LineTo(dc, mark.right - S(2), mark.top + S(3));
                    SelectObject(dc, oldCheck);
                    DeleteObject(checkPen);
                }
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(pen);
                wchar_t label[256] = {};
                GetWindowTextW(dis->hwndItem, label, 256);
                RECT textRc = rc;
                textRc.left = mark.right + S(6);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, disabled ? st->palette.muted : st->palette.text);
                HFONT font = reinterpret_cast<HFONT>(SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
                HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
                DrawTextW(dc, label, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (oldFont) SelectObject(dc, oldFont);
                return TRUE;
            }
            if (dis->CtlType == ODT_BUTTON) {
                const COLORREF bg = disabled ? st->palette.windowBg
                                             : (pressed ? st->palette.rowSelected : st->palette.panel);
                HBRUSH brush = CreateSolidBrush(bg);
                FillRect(dc, &rc, brush);
                DeleteObject(brush);
                HPEN pen = CreatePen(PS_SOLID, 1, st->palette.border);
                HGDIOBJ oldPen = SelectObject(dc, pen);
                HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(pen);
                wchar_t label[256] = {};
                GetWindowTextW(dis->hwndItem, label, 256);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, disabled ? st->palette.muted : st->palette.text);
                HFONT font = reinterpret_cast<HFONT>(SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
                HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
                if (pressed) OffsetRect(&rc, 1, 1);
                DrawTextW(dc, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (oldFont) SelectObject(dc, oldFont);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            if (!st) return 0;
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == IDM_TRAY_OPEN) {
                ShowMainWindow(st);
                return 0;
            }
            if (id == IDM_TRAY_EXIT) {
                st->exiting = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDC_COMPOSE_TO) {
                if (code == CBN_SELCHANGE) {
                    const int sel = ComboBox_GetCurSel(st->composeTo);
                    if (sel == 2) {
                        int idx = SelectedMessageIndex(st);
                        if (idx < 0) {
                            // Nothing selected: reply to the newest received message.
                            for (int i = static_cast<int>(st->messagesJson.size()) - 1; i >= 0; --i) {
                                if (st->messagesJson[static_cast<size_t>(i)].value(
                                        "direction", std::string()) != "out") {
                                    idx = i;
                                    break;
                                }
                            }
                            if (idx >= 0) {
                                SelectRowByData(st->messages, static_cast<LPARAM>(idx));
                                SelectMessage(st, idx);
                            }
                        }
                        EnterReplyMode(st, idx);
                        if (!st->replyActive) EnterEmptyReplyMode(st);
                    } else if (st->replyActive) {
                        ResetReply(st);
                        ComboBox_SetCurSel(st->composeTo, sel);
                    }
                    LayoutCurrentPage(st);
                }
                return 0;
            }
            if (code != BN_CLICKED) return 0;
            if (IsCheckboxControl(st, reinterpret_cast<HWND>(lParam))) {
                SetCheck(reinterpret_cast<HWND>(lParam), !GetCheck(reinterpret_cast<HWND>(lParam)));
                // IDC_SET_ENABLED also applies the change immediately below.
                if (id != IDC_SET_ENABLED) return 0;
            }
            switch (id) {
                case IDC_BTN_REFRESH_PEERS: {
                    nlohmann::json resp;
                    IpcCall({{"action", "chat_refresh"}}, resp);
                    SetTimer(hwnd, TIMER_PEERS, 800, nullptr);
                    break;
                }
                case IDC_BTN_DOWNLOAD:
                    DoDownload(st);
                    break;
                case IDC_BTN_OPEN_FOLDER:
                    DoOpenDownloadFolder(st);
                    break;
                case IDC_BTN_ATTACH:
                    DoAttach(st);
                    break;
                case IDC_BTN_REMOVE_ATTACH:
                    RemoveSelectedAttachment(st);
                    break;
                case IDC_BTN_SEND:
                    DoSend(st);
                    break;
                case IDC_BTN_LOG_REFRESH:
                    LoadLog(st);
                    break;
                case IDC_BTN_LOG_FOLDER: {
                    std::wstring dir = st->logDir.empty()
                        ? PathJoinWide(GetEffectiveDataDirectoryWide(), L"chat\\log")
                        : Utf8ToWide(st->logDir);
                    OpenFolder(dir);
                    break;
                }
                case IDC_BTN_APPLY:
                    DoApplySettings(st);
                    break;
                case IDC_SET_ENABLED: {
                    const bool on = GetCheck(st->setEnabled);
                    nlohmann::json resp;
                    if (!IpcCall({{"action", "chat_enable"}, {"enabled", on}}, resp) ||
                        !resp.value("ok", false)) {
                        std::string err = JsonError(resp, "IPC request failed");
                        MessageBoxW(hwnd, Utf8ToWide(err).c_str(),
                                    T(L"チャットを切り替えられません", L"Could not switch Chat",
                                      L"No se pudo cambiar el chat"),
                                    MB_ICONERROR | MB_OK);
                        SetCheck(st->setEnabled, !on);
                    } else {
                        if (st->setStatus) {
                            SetWindowTextW(st->setStatus,
                                           on ? T(L"有効", L"Enabled", L"Activado")
                                              : T(L"無効", L"Disabled", L"Desactivado"));
                        }
                        SetTimer(hwnd, TIMER_POLL, 300, nullptr);
                    }
                    break;
                }
                default:
                    break;
            }
            return 0;
        }
        case WM_NOTIFY: {
            if (!st) return 0;
            auto* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (!hdr) return 0;
            if (hdr->idFrom == IDC_TAB && hdr->code == TCN_SELCHANGE) {
                int page = TabCtrl_GetCurSel(st->hwndTab);
                ShowPage(st, page);
                if (page == 1) LoadLog(st);
                else if (page == 2) LoadSettings(st);
                return 0;
            }
            if (hdr->idFrom == IDC_PEERS && hdr->code == LVN_ITEMCHANGED) {
                auto* nv = reinterpret_cast<NMLISTVIEW*>(lParam);
                if (nv->iItem >= 0 && (nv->uChanged & LVIF_STATE) &&
                    (nv->uNewState & LVIS_SELECTED) && !(nv->uOldState & LVIS_SELECTED) &&
                    GetFocus() == st->peers) {
                    ResetReply(st);
                }
                return 0;
            }
            if (hdr->idFrom == IDC_MESSAGES && hdr->code == LVN_ITEMCHANGED) {
                auto* nv = reinterpret_cast<NMLISTVIEW*>(lParam);
                if (nv->iItem >= 0 && (nv->uChanged & LVIF_STATE) &&
                    (nv->uNewState & LVIS_SELECTED) && !(nv->uOldState & LVIS_SELECTED)) {
                    LVITEMW info = {};
                    info.mask = LVIF_PARAM;
                    info.iItem = nv->iItem;
                    if (ListView_GetItem(st->messages, &info)) {
                        const int idx = static_cast<int>(info.lParam);
                        SelectMessage(st, idx);
                        // A selected message means reply; a selected host means a new send.
                        if (st->replyActive || GetFocus() == st->messages) EnterReplyMode(st, idx);
                    }
                }
                return 0;
            }
            if (hdr->idFrom == IDC_MESSAGES && hdr->code == NM_CLICK) {
                auto* nm = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                if (nm && nm->iItem >= 0) {
                    LVITEMW info = {};
                    info.mask = LVIF_PARAM;
                    info.iItem = nm->iItem;
                    if (ListView_GetItem(st->messages, &info)) {
                        EnterReplyMode(st, static_cast<int>(info.lParam));
                    }
                }
                return 0;
            }
            if (hdr->idFrom == IDC_MESSAGES && hdr->code == NM_CUSTOMDRAW) {
                auto* cd = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    int row = static_cast<int>(cd->nmcd.dwItemSpec);
                    LVITEMW info = {};
                    info.mask = LVIF_PARAM;
                    info.iItem = row;
                    if (ListView_GetItem(st->messages, &info)) {
                        int idx = static_cast<int>(info.lParam);
                        if (idx >= 0 && static_cast<size_t>(idx) < st->messagesJson.size()) {
                            const auto& m = st->messagesJson[static_cast<size_t>(idx)];
                            if (!m.value("read", false) &&
                                m.value("direction", std::string()) != "out") {
                                cd->clrText = RGB(0xB0, 0x30, 0x30);
                            }
                        }
                    }
                    return CDRF_DODEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            return 0;
        }
        case kTrayMessage: {
            if (!st) return 0;
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP:
                case NIN_SELECT:
                case NIN_BALLOONUSERCLICK:
                    ShowMainWindow(st);
                    break;
                case WM_RBUTTONUP:
                    ShowTrayMenu(st);
                    break;
                default:
                    break;
            }
            return 0;
        }
        case WM_CLOSE:
            if (st && !st->exiting) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_ENDSESSION:
            if (st && wParam) {
                st->exiting = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_POLL);
            KillTimer(hwnd, TIMER_PEERS);
            KillTimer(hwnd, TIMER_RELOAD);
            if (st) {
                RemoveTrayIcon(st);
                RemovePropW(hwnd, kPropDataDir);
                if (st->dataDirAtom) {
                    GlobalDeleteAtom(st->dataDirAtom);
                    st->dataDirAtom = 0;
                }
                if (st->font) {
                    DeleteObject(st->font);
                    st->font = nullptr;
                }
                DestroyDialogTheme(st);
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND FindExistingDialog() {
    std::wstring want = GetEffectiveDataDirectoryWide();
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, kWndClass, nullptr)) != nullptr) {
        HANDLE prop = GetPropW(h, kPropDataDir);
        if (!prop) continue;
        wchar_t buf[1024] = {};
        UINT n = GlobalGetAtomNameW(static_cast<ATOM>(reinterpret_cast<ULONG_PTR>(prop)),
                                    buf, 1024);
        if (n > 0 && _wcsicmp(buf, want.c_str()) == 0) return h;
    }
    return nullptr;
}

} // namespace

bool ActivateIpmsgDialog() {
    HWND h = FindExistingDialog();
    if (!h) return false;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    else ShowWindow(h, SW_SHOW);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    return true;
}

int RunIpmsgDialog(bool startHidden) {
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES |
                 ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    g_lang = DetectLanguage();
    InitDashboardDarkMode(GetDashboardPalette().dark);
    g_showMessage = RegisterWindowMessageW(L"HanchoIpmsgShow");

    std::wstring mutexName = GetGuiMutexNameWide() + L"_ipmsg";
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindExistingDialog()) {
            PostMessageW(other, g_showMessage, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    DialogState st;
    st.mutex = mutex;
    st.startHidden = startHidden;
    st.startedMs = static_cast<int64_t>(GetNowTickMs());
    st.dpi = SystemDpi();
    st.inst = GetModuleHandleW(nullptr);
    g_state = &st;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = IpmsgWndProc;
    wc.hInstance = st.inst;
    wc.hIcon = LoadIconW(st.inst, MAKEINTRESOURCEW(2));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kWndClass, L"Hancho Chat",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820,
                                nullptr, nullptr, st.inst, &st);
    if (!hwnd) {
        g_state = nullptr;
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    if (!startHidden) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        try {
            DispatchMessageW(&msg);
        } catch (const std::exception& e) {
            WriteAuditLog("CRASH_GUARD", std::string("Chat GUI exception: ") + e.what());
        } catch (...) {
            WriteAuditLog("CRASH_GUARD", "Chat GUI exception: unknown");
        }
    }

    g_state = nullptr;
    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}

} // namespace pc

