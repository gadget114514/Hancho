#include "ipmsg.h"

#include "common.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace pc {

namespace {

// --- IPMsg protocol constants (ipmsg.h of the reference implementation) ---
constexpr uint32_t IPMSG_VERSION = 0x0001;
constexpr uint32_t IPMSG_NOOPERATION = 0x00;
constexpr uint32_t IPMSG_BR_ENTRY = 0x01;
constexpr uint32_t IPMSG_BR_EXIT = 0x02;
constexpr uint32_t IPMSG_ANSENTRY = 0x03;
constexpr uint32_t IPMSG_BR_ABSENCE = 0x04;
constexpr uint32_t IPMSG_SENDMSG = 0x20;
constexpr uint32_t IPMSG_RECVMSG = 0x21;
constexpr uint32_t IPMSG_READMSG = 0x30;
constexpr uint32_t IPMSG_DELMSG = 0x31;
constexpr uint32_t IPMSG_ANSREADMSG = 0x32;
constexpr uint32_t IPMSG_GETINFO = 0x40;
constexpr uint32_t IPMSG_SENDINFO = 0x41;
constexpr uint32_t IPMSG_GETABSENCEINFO = 0x50;
constexpr uint32_t IPMSG_SENDABSENCEINFO = 0x51;
constexpr uint32_t IPMSG_GETFILEDATA = 0x60;
constexpr uint32_t IPMSG_RELEASEFILES = 0x61;

constexpr uint32_t IPMSG_ABSENCEOPT = 0x00000100;
constexpr uint32_t IPMSG_SENDCHECKOPT = 0x00000100;
constexpr uint32_t IPMSG_SECRETOPT = 0x00000200;
constexpr uint32_t IPMSG_BROADCASTOPT = 0x00000400;
constexpr uint32_t IPMSG_MULTICASTOPT = 0x00000800;
constexpr uint32_t IPMSG_AUTORETOPT = 0x00002000;
constexpr uint32_t IPMSG_READCHECKOPT = 0x00100000;
constexpr uint32_t IPMSG_FILEATTACHOPT = 0x00200000;
constexpr uint32_t IPMSG_UTF8OPT = 0x00800000;
constexpr uint32_t IPMSG_CAPUTF8OPT = 0x01000000;

constexpr uint32_t IPMSG_FILE_REGULAR = 0x00000001;

inline uint32_t GetMode(uint32_t cmd) { return cmd & 0x000000ffu; }

constexpr const char* kHanchoExtTag = "HANCHO1";
constexpr size_t kMaxUdpPacket = 65000;
constexpr int kRetryIntervalMs = 2000;
constexpr int kMaxSendTries = 3;
constexpr int64_t kOfferTtlMs = 24ll * 3600 * 1000;

#ifdef _WIN32
using socket_t = SOCKET;
const socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
const socket_t kInvalidSocket = -1;
#endif

void EnsureNetworking() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data = {};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

void CloseSock(socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

int LastSockError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Waits until s is readable (or writable). Returns >0 ready, 0 timeout, <0 error.
int WaitSock(socket_t s, int timeoutMs, bool forWrite = false) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
    return select(0, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &tv);
#else
    return select(s + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &tv);
#endif
}

bool SendAll(socket_t s, const char* data, size_t len, const std::atomic<bool>& stop) {
    size_t off = 0;
    while (off < len) {
        if (stop.load()) return false;
        int w = WaitSock(s, 1000, true);
        if (w < 0) return false;
        if (w == 0) continue;
        int chunk = static_cast<int>(std::min<size_t>(len - off, 1 << 20));
        int n = ::send(s, data + off, chunk, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string FormatLocalTime(int64_t ms) {
    time_t t = static_cast<time_t>(ms / 1000);
    std::tm tmv = {};
    if (!LocalTimeFromSec(&t, &tmv)) return std::string();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

// --- Text encodings: IPMsg without UTF8OPT carries CP932 (Shift_JIS) ---
std::string Cp932ToUtf8(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return s;
    int wlen = MultiByteToWideChar(932, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wlen <= 0) return s;
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(932, 0, s.data(), static_cast<int>(s.size()), &w[0], wlen);
    return WideToUtf8(w);
#else
    return s;
#endif
}

std::string Utf8ToCp932(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return s;
    std::wstring w = Utf8ToWide(s);
    int len = WideCharToMultiByte(932, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return s;
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(932, 0, w.data(), static_cast<int>(w.size()), &out[0], len, nullptr, nullptr);
    return out;
#else
    return s;
#endif
}

std::string StripColons(std::string s) {
    // Header fields are ':'-separated; IPMsg has no escape for them.
    for (char& c : s) if (c == ':' || c == '\0') c = '_';
    return s;
}

std::string ToHex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
    return buf;
}

bool ParseHex(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 16);
    if (!end || *end != '\0') return false;
    out = v;
    return true;
}

bool ParseDec(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = v;
    return true;
}

std::vector<std::string> SplitOn(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

// --- Address handling ---
bool ParseIpv4(const std::string& s, uint32_t& outHostOrder) {
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    outHostOrder = ntohl(a.s_addr);
    return true;
}

std::string Ipv4ToString(uint32_t hostOrder) {
    in_addr a{};
    a.s_addr = htonl(hostOrder);
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

bool InCidr(uint32_t addr, uint32_t net, int bits) {
    if (bits <= 0) return true;
    uint32_t mask = bits >= 32 ? 0xffffffffu : ~((1u << (32 - bits)) - 1u);
    return (addr & mask) == (net & mask);
}

// Loopback, RFC 1918, link-local and CGNAT (VPN overlays such as Tailscale).
bool IsLanAddress(uint32_t a) {
    return InCidr(a, 0x7f000000u, 8) ||     // 127.0.0.0/8
           InCidr(a, 0x0a000000u, 8) ||     // 10.0.0.0/8
           InCidr(a, 0xac100000u, 12) ||    // 172.16.0.0/12
           InCidr(a, 0xc0a80000u, 16) ||    // 192.168.0.0/16
           InCidr(a, 0xa9fe0000u, 16) ||    // 169.254.0.0/16
           InCidr(a, 0x64400000u, 10);      // 100.64.0.0/10
}

struct Cidr { uint32_t net = 0; int bits = 32; };

bool ParseCidr(const std::string& s, Cidr& out) {
    std::string addr = s;
    int bits = 32;
    size_t slash = s.find('/');
    if (slash != std::string::npos) {
        addr = s.substr(0, slash);
        try { bits = std::stoi(s.substr(slash + 1)); } catch (...) { return false; }
        if (bits < 0 || bits > 32) return false;
    }
    uint32_t a = 0;
    if (!ParseIpv4(Trim(addr), a)) return false;
    out.net = a;
    out.bits = bits;
    return true;
}

std::string LocalUserName() {
    const char* names[] = {"USERNAME", "USER", "LOGNAME"};
    for (const char* n : names) {
#ifdef _WIN32
        char* v = nullptr;
        size_t len = 0;
        if (_dupenv_s(&v, &len, n) == 0 && v) {
            std::string s = v;
            free(v);
            if (!s.empty()) return s;
        }
#else
        const char* v = std::getenv(n);
        if (v && *v) return v;
#endif
    }
    return "hancho";
}

std::string LocalHostName() {
    EnsureNetworking();
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return buf;
    return "localhost";
}

FILE* OpenFileUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    FILE* f = nullptr;
    std::wstring wmode = Utf8ToWide(mode);
    if (_wfopen_s(&f, Utf8ToWide(path).c_str(), wmode.c_str()) != 0) return nullptr;
    return f;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

int64_t FileSeek(FILE* f, int64_t off) {
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET);
#else
    return fseeko(f, static_cast<off_t>(off), SEEK_SET);
#endif
}

std::string SafeFileName(const std::string& name) {
    // Keep only the last path component and drop characters Windows rejects.
    std::string base = name;
    size_t p = base.find_last_of("/\\");
    if (p != std::string::npos) base = base.substr(p + 1);
    std::string out;
    for (char c : base) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty() || out == "." || out == "..") out = "file";
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

IpmsgConfig IpmsgConfig::FromJson(const nlohmann::json& j) {
    IpmsgConfig c;
    if (!j.is_object()) return c;
    if (j.contains("enabled") && j["enabled"].is_boolean()) c.enabled = j["enabled"].get<bool>();
    if (j.contains("port") && j["port"].is_number_integer()) {
        int64_t p = j["port"].get<int64_t>();
        if (p > 0 && p < 65536) c.port = static_cast<uint16_t>(p);
    }
    if (j.contains("bind") && j["bind"].is_string() && !j["bind"].get<std::string>().empty())
        c.bind = j["bind"].get<std::string>();
    if (j.contains("nickname") && j["nickname"].is_string()) c.nickname = j["nickname"].get<std::string>();
    if (j.contains("group") && j["group"].is_string()) c.group = j["group"].get<std::string>();
    auto readList = [&](const char* key, std::vector<std::string>& out) {
        if (!j.contains(key)) return;
        const auto& v = j[key];
        if (v.is_array()) {
            for (const auto& e : v) if (e.is_string() && !Trim(e.get<std::string>()).empty()) out.push_back(Trim(e.get<std::string>()));
        } else if (v.is_string()) {
            for (auto& part : SplitOn(v.get<std::string>(), ',')) {
                std::string t = Trim(part);
                if (!t.empty()) out.push_back(t);
            }
        }
    };
    readList("hosts", c.hosts);
    readList("allow_from", c.allowFrom);
    if (j.contains("download_dir") && j["download_dir"].is_string()) c.downloadDir = j["download_dir"].get<std::string>();
    if (j.contains("auto_download") && j["auto_download"].is_boolean()) c.autoDownload = j["auto_download"].get<bool>();
    if (j.contains("max_file_mb") && j["max_file_mb"].is_number()) {
        double mb = j["max_file_mb"].get<double>();
        if (mb > 0) c.maxFileBytes = static_cast<int64_t>(mb * 1024 * 1024);
    }
    if (j.contains("max_messages") && j["max_messages"].is_number_integer()) {
        int64_t m = j["max_messages"].get<int64_t>();
        if (m >= 10 && m <= 100000) c.maxMessages = static_cast<size_t>(m);
    }
    return c;
}

nlohmann::json IpmsgConfig::ToJson() const {
    nlohmann::json j;
    j["enabled"] = enabled;
    j["port"] = port;
    j["bind"] = bind;
    j["nickname"] = nickname;
    j["group"] = group;
    j["hosts"] = hosts;
    j["allow_from"] = allowFrom;
    j["download_dir"] = downloadDir;
    j["auto_download"] = autoDownload;
    j["max_file_mb"] = static_cast<double>(maxFileBytes) / (1024.0 * 1024.0);
    j["max_messages"] = maxMessages;
    return j;
}

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

struct IpmsgNode::Impl {
    struct Peer {
        std::string key;  // "ip:port"
        std::string addr;
        uint16_t port = 0;
        std::string user, host, nickname, group;
        bool capUtf8 = false;
        bool absent = false;
        bool hancho = false;
        int64_t lastSeenMs = 0;
        std::string info;
    };
    struct Attachment {
        uint64_t fileId = 0;
        std::string name;
        int64_t size = 0;
        int64_t mtime = 0;
        uint32_t attr = IPMSG_FILE_REGULAR;
        std::string localPath;   // outgoing source / incoming destination
        std::string status;      // out: offered|sent ; in: available|downloading|done|failed|unsupported
        int64_t bytes = 0;
        std::string error;
    };
    struct Message {
        uint64_t id = 0;
        int64_t timeMs = 0;
        std::string direction;  // in | out | local
        std::string peerKey, peerAddr, peerUser, peerHost, peerNick;
        uint16_t peerPort = 0;
        std::string text;
        std::string fromAgent, toAgent;
        uint64_t replyTo = 0;
        uint64_t packetNo = 0;
        bool broadcast = false;
        bool read = false;
        bool readCheck = false;  // sender asked for READMSG
        std::string status;      // out: sending|delivered|read|failed ; in/local: received
        std::string audience;    // human | agent | "" (unspecified)
        int hops = 0;
        std::vector<Attachment> files;
        // Retry bookkeeping for outgoing (not persisted)
        std::string rawPacket;
        int tries = 0;
        int64_t lastSendMs = 0;
    };
    struct Offer {
        uint64_t packetNo = 0;
        std::set<std::string> peerAddrs;  // empty = any allowed address
        std::vector<Attachment> files;
        int64_t createdMs = 0;
    };
    struct Worker {
        std::thread th;
        std::shared_ptr<std::atomic<bool>> done;
    };

    IpmsgNode* self = nullptr;
    mutable std::mutex mu;
    std::condition_variable cv;
    IpmsgConfig cfg;
    std::vector<Cidr> allow;
    std::string selfUser, selfHost, selfNick;
    std::map<std::string, Peer> peers;
    std::deque<Message> messages;
    uint64_t nextId = 1;
    uint64_t nextPacketNo = 1;
    std::map<uint64_t, Offer> offers;
    std::map<std::string, int64_t> seenPackets;  // "key#pkt" -> time, duplicate filter
    bool dirty = false;
    int64_t lastSaveMs = 0;
    bool loaded = false;

    socket_t udp = kInvalidSocket;
    socket_t tcp = kInvalidSocket;
    std::atomic<bool> stop{false};
    std::thread udpThread;
    std::thread tcpThread;
    std::mutex workersMu;
    std::vector<Worker> workers;
    std::mutex sendMu;
    std::mutex cbMu;
    std::function<void(const nlohmann::json&)> onMessage;
    mutable std::mutex logMu;

    std::wstring LogDir() const { return PathJoinWide(StoreDir(), L"log"); }

    // Append-only conversation log: one JSON line per event in
    // <data-dir>/ipmsg/log/ipmsg-YYYY-MM-DD.jsonl (never pruned by max_messages).
    void LogEvent(const std::string& event, const nlohmann::json& body) {
        try {
            nlohmann::json j = body;
            const int64_t now = NowMs();
            j["event"] = event;
            j["logged_at"] = FormatLocalTime(now);
            j["logged_ms"] = now;
            std::string day = FormatLocalTime(now).substr(0, 10);
            std::lock_guard<std::mutex> lk(logMu);
            EnsureDirectoryWide(LogDir());
            AppendToLogFileWide(PathJoinWide(LogDir(), Utf8ToWide("ipmsg-" + day + ".jsonl")),
                                j.dump() + "\n");
        } catch (...) {}
    }

    void Notify(const nlohmann::json& msgJson) {
        std::function<void(const nlohmann::json&)> cb;
        {
            std::lock_guard<std::mutex> lk(cbMu);
            cb = onMessage;
        }
        if (cb) {
            try { cb(msgJson); } catch (...) {}
        }
    }

    std::wstring StoreDir() const {
        return PathJoinWide(GetEffectiveDataDirectoryWide(), L"ipmsg");
    }
    std::wstring StorePath() const { return PathJoinWide(StoreDir(), L"messages.json"); }
    std::string DownloadDir() const {
        if (!cfg.downloadDir.empty()) return cfg.downloadDir;
        return WideToUtf8(PathJoinWide(StoreDir(), L"files"));
    }

    void Touch() {
        self->version_.fetch_add(1);
        cv.notify_all();
    }

    bool IsAllowed(uint32_t addr) const {
        if (!IsLanAddress(addr)) return false;
        if (allow.empty()) return true;
        for (const auto& c : allow) if (InCidr(addr, c.net, c.bits)) return true;
        return false;
    }

    // --- Packet building ---
    uint64_t NewPacketNo() { return nextPacketNo++; }

    std::string Header(uint64_t pkt, uint32_t cmd) const {
        std::ostringstream o;
        o << IPMSG_VERSION << ":" << pkt << ":" << StripColons(selfUser) << ":"
          << StripColons(selfHost) << ":" << cmd << ":";
        return o.str();
    }

    std::string EntryPacket(uint32_t mode) {
        uint32_t cmd = mode | IPMSG_CAPUTF8OPT | IPMSG_FILEATTACHOPT;
        std::string p = Header(NewPacketNo(), cmd);
        p += Utf8ToCp932(selfNick);
        p.push_back('\0');
        p += Utf8ToCp932(cfg.group);
        p.push_back('\0');
        // UTF-8 entry extension understood by IPMsg 3.x and later.
        p += "\nUN:" + selfUser + "\nHN:" + selfHost + "\nNN:" + selfNick +
             "\nGN:" + cfg.group + "\nHC:" + std::string(PC_VERSION) + "\n";
        return p;
    }

    void SendTo(const std::string& data, uint32_t addrHostOrder, uint16_t port) {
        if (udp == kInvalidSocket) return;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = htonl(addrHostOrder);
        std::lock_guard<std::mutex> lk(sendMu);
        ::sendto(udp, data.data(), static_cast<int>(data.size()), 0,
                 reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }

    void SendToKey(const std::string& data, const std::string& addr, uint16_t port) {
        uint32_t a = 0;
        if (ParseIpv4(addr, a)) SendTo(data, a, port);
    }

    // Broadcast + configured hosts.
    void SendEntryTargets(const std::string& data) {
        SendTo(data, 0xffffffffu, cfg.port);
        for (const auto& h : cfg.hosts) {
            std::string addr = h;
            uint16_t port = cfg.port;
            size_t colon = h.rfind(':');
            if (colon != std::string::npos) {
                addr = h.substr(0, colon);
                try {
                    int p = std::stoi(h.substr(colon + 1));
                    if (p > 0 && p < 65536) port = static_cast<uint16_t>(p);
                } catch (...) {}
            }
            uint32_t a = 0;
            if (ParseIpv4(Trim(addr), a)) {
                SendTo(data, a, port);
            } else {
                // Host name: resolve once per send.
                addrinfo hints{};
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_DGRAM;
                addrinfo* res = nullptr;
                if (getaddrinfo(Trim(addr).c_str(), nullptr, &hints, &res) == 0 && res) {
                    auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
                    SendTo(data, ntohl(sin->sin_addr.s_addr), port);
                    freeaddrinfo(res);
                }
            }
        }
    }

    // --- Persistence ---
    static nlohmann::json AttachmentJson(const Attachment& a) {
        nlohmann::json j;
        j["file_id"] = a.fileId;
        j["name"] = a.name;
        j["size"] = a.size;
        j["mtime"] = a.mtime;
        j["status"] = a.status;
        if (a.bytes) j["bytes"] = a.bytes;
        if (!a.localPath.empty()) j["path"] = a.localPath;
        if (!a.error.empty()) j["error"] = a.error;
        return j;
    }

    nlohmann::json MessageJson(const Message& m) const {
        nlohmann::json j;
        j["id"] = m.id;
        j["time"] = FormatLocalTime(m.timeMs);
        j["time_ms"] = m.timeMs;
        j["direction"] = m.direction;
        nlohmann::json p;
        p["key"] = m.peerKey;
        p["addr"] = m.peerAddr;
        p["port"] = m.peerPort;
        p["user"] = m.peerUser;
        p["host"] = m.peerHost;
        p["nickname"] = m.peerNick;
        j["peer"] = p;
        j["text"] = m.text;
        if (!m.fromAgent.empty()) j["from_agent"] = m.fromAgent;
        if (!m.toAgent.empty()) j["to_agent"] = m.toAgent;
        if (m.replyTo) j["reply_to"] = m.replyTo;
        j["status"] = m.status;
        j["read"] = m.read;
        if (m.broadcast) j["broadcast"] = true;
        if (!m.audience.empty()) j["audience"] = m.audience;
        if (m.hops) j["hops"] = m.hops;
        if (!m.files.empty()) {
            nlohmann::json fs = nlohmann::json::array();
            for (const auto& a : m.files) fs.push_back(AttachmentJson(a));
            j["files"] = fs;
        }
        return j;
    }

    void Load() {
        if (loaded) return;
        loaded = true;
        std::string content = ReadFileUtf8Wide(StorePath());
        if (content.empty()) return;
        nlohmann::json root = nlohmann::json::parse(content, nullptr, false);
        if (!root.is_object() || !root.contains("messages") || !root["messages"].is_array()) return;
        for (const auto& j : root["messages"]) {
            if (!j.is_object()) continue;
            Message m;
            m.id = j.value("id", uint64_t(0));
            if (!m.id) continue;
            m.timeMs = j.value("time_ms", int64_t(0));
            m.direction = j.value("direction", std::string("in"));
            if (j.contains("peer") && j["peer"].is_object()) {
                const auto& p = j["peer"];
                m.peerKey = p.value("key", std::string());
                m.peerAddr = p.value("addr", std::string());
                m.peerPort = static_cast<uint16_t>(p.value("port", 0));
                m.peerUser = p.value("user", std::string());
                m.peerHost = p.value("host", std::string());
                m.peerNick = p.value("nickname", std::string());
            }
            m.text = j.value("text", std::string());
            m.fromAgent = j.value("from_agent", std::string());
            m.toAgent = j.value("to_agent", std::string());
            m.replyTo = j.value("reply_to", uint64_t(0));
            m.status = j.value("status", std::string());
            if (m.status == "sending") m.status = "failed";
            m.read = j.value("read", false);
            m.broadcast = j.value("broadcast", false);
            m.audience = j.value("audience", std::string());
            m.hops = j.value("hops", 0);
            if (j.contains("files") && j["files"].is_array()) {
                for (const auto& f : j["files"]) {
                    Attachment a;
                    a.fileId = f.value("file_id", uint64_t(0));
                    a.name = f.value("name", std::string());
                    a.size = f.value("size", int64_t(0));
                    a.mtime = f.value("mtime", int64_t(0));
                    a.status = f.value("status", std::string());
                    a.bytes = f.value("bytes", int64_t(0));
                    a.localPath = f.value("path", std::string());
                    a.error = f.value("error", std::string());
                    // Offers and transfers do not survive a restart.
                    if (a.status == "downloading") { a.status = "failed"; a.error = "interrupted"; }
                    if (a.status == "available" && m.direction == "in") a.status = "expired";
                    if (a.status == "offered") a.status = "expired";
                    m.files.push_back(std::move(a));
                }
            }
            nextId = std::max(nextId, m.id + 1);
            messages.push_back(std::move(m));
        }
    }

    void SaveLocked() {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& m : messages) arr.push_back(MessageJson(m));
        nlohmann::json root;
        root["version"] = 1;
        root["messages"] = arr;
        EnsureDirectoryWide(StoreDir());
        WriteFileAtomicWide(StorePath(), root.dump(1));
        dirty = false;
        lastSaveMs = NowMs();
    }

    Message& AddMessageLocked(Message m) {
        m.id = nextId++;
        if (!m.timeMs) m.timeMs = NowMs();
        messages.push_back(std::move(m));
        while (messages.size() > cfg.maxMessages) messages.pop_front();
        dirty = true;
        Touch();
        return messages.back();
    }

    Message* FindMessageLocked(uint64_t id) {
        for (auto& m : messages) if (m.id == id) return &m;
        return nullptr;
    }

    // --- Receiving ---
    void HandlePacket(const std::string& raw, uint32_t fromAddr, uint16_t fromPort) {
        // "ver:pkt:user:host:cmd:extra"
        size_t pos[5];
        size_t p = 0;
        for (int i = 0; i < 5; ++i) {
            size_t c = raw.find(':', p);
            if (c == std::string::npos) return;
            pos[i] = c;
            p = c + 1;
        }
        std::string verS = raw.substr(0, pos[0]);
        std::string pktS = raw.substr(pos[0] + 1, pos[1] - pos[0] - 1);
        std::string userRaw = raw.substr(pos[1] + 1, pos[2] - pos[1] - 1);
        std::string hostRaw = raw.substr(pos[2] + 1, pos[3] - pos[2] - 1);
        std::string cmdS = raw.substr(pos[3] + 1, pos[4] - pos[3] - 1);
        std::string extra = raw.substr(pos[4] + 1);
        uint64_t ver = 0, pkt = 0, cmd64 = 0;
        if (!ParseDec(verS, ver) || !ParseDec(cmdS, cmd64)) return;
        ParseDec(pktS, pkt);  // some clients send non-numeric ids; keep 0
        const uint32_t cmd = static_cast<uint32_t>(cmd64);
        const uint32_t mode = GetMode(cmd);
        const bool utf8 = (cmd & IPMSG_UTF8OPT) != 0;
        auto dec = [&](const std::string& s) { return utf8 ? s : Cp932ToUtf8(s); };
        const std::string user = dec(userRaw);
        const std::string host = dec(hostRaw);
        const std::string addr = Ipv4ToString(fromAddr);
        const std::string key = addr + ":" + std::to_string(fromPort);

        // Our own broadcast echo.
        if (user == selfUser && host == selfHost && fromPort == cfg.port) return;

        std::unique_lock<std::mutex> lk(mu);
        auto upsertPeer = [&](bool fromEntry) -> Peer& {
            Peer& pr = peers[key];
            pr.key = key;
            pr.addr = addr;
            pr.port = fromPort;
            pr.user = user;
            pr.host = host;
            pr.lastSeenMs = NowMs();
            if (fromEntry) {
                pr.capUtf8 = (cmd & IPMSG_CAPUTF8OPT) != 0;
                pr.absent = (cmd & IPMSG_ABSENCEOPT) != 0;
                std::vector<std::string> parts;
                {
                    size_t s = 0;
                    for (int i = 0; i < 2; ++i) {
                        size_t z = extra.find('\0', s);
                        if (z == std::string::npos) { parts.push_back(extra.substr(s)); s = extra.size(); break; }
                        parts.push_back(extra.substr(s, z - s));
                        s = z + 1;
                    }
                    parts.push_back(s < extra.size() ? extra.substr(s) : std::string());
                }
                pr.nickname = parts.size() > 0 ? dec(parts[0]) : std::string();
                pr.group = parts.size() > 1 ? dec(parts[1]) : std::string();
                const std::string ext = parts.size() > 2 ? parts[2] : std::string();
                pr.hancho = false;
                for (const auto& line : SplitOn(ext, '\n')) {
                    if (line.rfind("NN:", 0) == 0) pr.nickname = line.substr(3);
                    else if (line.rfind("GN:", 0) == 0) pr.group = line.substr(3);
                    else if (line.rfind("HC:", 0) == 0) pr.hancho = true;
                }
                if (!pr.capUtf8 && ext.find("NN:") != std::string::npos) pr.capUtf8 = true;
            }
            if (pr.nickname.empty()) pr.nickname = user;
            return pr;
        };

        switch (mode) {
        case IPMSG_NOOPERATION:
            return;
        case IPMSG_BR_ENTRY: {
            upsertPeer(true);
            Touch();
            std::string reply = EntryPacket(IPMSG_ANSENTRY);
            lk.unlock();
            SendTo(reply, fromAddr, fromPort);
            return;
        }
        case IPMSG_ANSENTRY:
        case IPMSG_BR_ABSENCE:
            upsertPeer(true);
            Touch();
            return;
        case IPMSG_BR_EXIT:
            peers.erase(key);
            Touch();
            return;
        case IPMSG_SENDMSG: {
            Peer& pr = upsertPeer(false);
            const bool wantAck = (cmd & IPMSG_SENDCHECKOPT) && !(cmd & (IPMSG_BROADCASTOPT | IPMSG_AUTORETOPT));
            std::string ack;
            if (wantAck) ack = Header(NewPacketNo(), IPMSG_RECVMSG) + std::to_string(pkt);
            const std::string dupKey = key + "#" + pktS;
            const bool dup = seenPackets.count(dupKey) > 0;
            seenPackets[dupKey] = NowMs();
            if (!dup) {
                // text \0 filelist \0 HANCHO1{json}
                std::string text = extra, fileList, ext;
                size_t z1 = extra.find('\0');
                if (z1 != std::string::npos) {
                    text = extra.substr(0, z1);
                    size_t z2 = extra.find('\0', z1 + 1);
                    fileList = extra.substr(z1 + 1, z2 == std::string::npos ? std::string::npos : z2 - z1 - 1);
                    if (z2 != std::string::npos) {
                        ext = extra.substr(z2 + 1);
                        size_t z3 = ext.find('\0');
                        if (z3 != std::string::npos) ext.resize(z3);
                    }
                }
                Message m;
                m.direction = "in";
                m.peerKey = key;
                m.peerAddr = addr;
                m.peerPort = fromPort;
                m.peerUser = user;
                m.peerHost = host;
                m.peerNick = pr.nickname;
                m.text = dec(text);
                // IPMsg uses CRLF on the wire; keep LF internally.
                std::string norm;
                norm.reserve(m.text.size());
                for (size_t i = 0; i < m.text.size(); ++i) {
                    if (m.text[i] == '\r' && i + 1 < m.text.size() && m.text[i + 1] == '\n') continue;
                    norm.push_back(m.text[i]);
                }
                m.text = std::move(norm);
                m.packetNo = pkt;
                m.broadcast = (cmd & IPMSG_BROADCASTOPT) != 0;
                m.readCheck = (cmd & IPMSG_SECRETOPT) && (cmd & IPMSG_READCHECKOPT);
                m.status = "received";
                if (ext.rfind(kHanchoExtTag, 0) == 0) {
                    nlohmann::json ej = nlohmann::json::parse(ext.substr(std::strlen(kHanchoExtTag)), nullptr, false);
                    if (ej.is_object()) {
                        m.fromAgent = ej.value("from_agent", std::string());
                        m.toAgent = ej.value("to_agent", std::string());
                        m.replyTo = ej.value("reply_to", uint64_t(0));
                        m.audience = ej.value("audience", std::string());
                        if (m.audience != "human" && m.audience != "agent") m.audience.clear();
                        m.hops = ej.value("hops", 0);
                    }
                }
                if (m.toAgent.empty() && m.text.size() > 1 && m.text[0] == '@') {
                    size_t sp = m.text.find_first_of(" \t\n");
                    if (sp != std::string::npos && sp > 1) m.toAgent = m.text.substr(1, sp - 1);
                }
                if ((cmd & IPMSG_FILEATTACHOPT) && !fileList.empty()) {
                    for (const auto& entry : SplitOn(fileList, '\a')) {
                        if (entry.empty()) continue;
                        // fileID:filename:size:mtime:attr[:ext...]  ("::" escapes ':' in names)
                        std::vector<std::string> f;
                        std::string cur;
                        for (size_t i = 0; i < entry.size(); ++i) {
                            if (entry[i] == ':') {
                                if (f.size() == 1 && i + 1 < entry.size() && entry[i + 1] == ':') {
                                    cur.push_back(':');
                                    ++i;
                                    continue;
                                }
                                f.push_back(cur);
                                cur.clear();
                            } else {
                                cur.push_back(entry[i]);
                            }
                        }
                        f.push_back(cur);
                        if (f.size() < 5) continue;
                        Attachment a;
                        uint64_t v = 0;
                        if (!ParseHex(Trim(f[0]), v)) continue;
                        a.fileId = v;
                        a.name = dec(f[1]);
                        if (ParseHex(f[2], v)) a.size = static_cast<int64_t>(v);
                        if (ParseHex(f[3], v)) a.mtime = static_cast<int64_t>(v);
                        if (ParseHex(f[4], v)) a.attr = static_cast<uint32_t>(v);
                        a.status = ((a.attr & 0xff) == IPMSG_FILE_REGULAR) ? "available" : "unsupported";
                        m.files.push_back(std::move(a));
                    }
                }
                Message& stored = AddMessageLocked(std::move(m));
                const uint64_t newId = stored.id;
                const bool autoDl = cfg.autoDownload && !stored.files.empty();
                nlohmann::json mj = MessageJson(stored);
                lk.unlock();
                if (!ack.empty()) SendTo(ack, fromAddr, fromPort);
                LogEvent("recv", mj);
                Notify(mj);
                if (autoDl) StartDownloads(newId, -1, std::string());
                return;
            }
            lk.unlock();
            if (!ack.empty()) SendTo(ack, fromAddr, fromPort);
            return;
        }
        case IPMSG_RECVMSG:
        case IPMSG_READMSG:
        case IPMSG_ANSREADMSG: {
            uint64_t ref = 0;
            std::string refS = extra.substr(0, extra.find('\0'));
            if (!ParseDec(Trim(refS), ref)) return;
            for (auto& m : messages) {
                if (m.direction == "out" && m.packetNo == ref && m.peerKey == key) {
                    if (mode == IPMSG_RECVMSG) {
                        if (m.status == "sending" || m.status == "failed") m.status = "delivered";
                    } else {
                        m.status = "read";
                    }
                    m.rawPacket.clear();
                    dirty = true;
                    Touch();
                    nlohmann::json ev = {{"id", m.id}, {"peer", m.peerKey}, {"status", m.status}};
                    lk.unlock();
                    LogEvent("status", ev);
                    return;
                }
            }
            return;
        }
        case IPMSG_DELMSG:
            return;
        case IPMSG_GETINFO: {
            std::string reply = Header(NewPacketNo(), IPMSG_SENDINFO) +
                                "Hancho " + std::string(PC_VERSION) + " (IPMsg compatible)";
            lk.unlock();
            SendTo(reply, fromAddr, fromPort);
            return;
        }
        case IPMSG_SENDINFO: {
            auto it = peers.find(key);
            if (it != peers.end()) {
                it->second.info = dec(extra.substr(0, extra.find('\0')));
                Touch();
            }
            return;
        }
        case IPMSG_GETABSENCEINFO: {
            std::string reply = Header(NewPacketNo(), IPMSG_SENDABSENCEINFO) + "Not absence mode";
            lk.unlock();
            SendTo(reply, fromAddr, fromPort);
            return;
        }
        case IPMSG_RELEASEFILES: {
            uint64_t ref = 0;
            if (ParseDec(Trim(extra.substr(0, extra.find('\0'))), ref)) {
                auto it = offers.find(ref);
                if (it != offers.end()) {
                    it->second.peerAddrs.erase(addr);
                    if (it->second.peerAddrs.empty()) offers.erase(it);
                }
            }
            return;
        }
        default:
            return;
        }
    }

    void Maintenance() {
        std::vector<std::pair<std::string, std::pair<std::string, uint16_t>>> resend;
        std::vector<nlohmann::json> failedEvents;
        {
            std::lock_guard<std::mutex> lk(mu);
            const int64_t now = NowMs();
            for (auto& m : messages) {
                if (m.direction != "out" || m.status != "sending" || m.rawPacket.empty()) continue;
                if (now - m.lastSendMs < kRetryIntervalMs) continue;
                if (m.tries >= kMaxSendTries) {
                    m.status = "failed";
                    m.rawPacket.clear();
                    dirty = true;
                    Touch();
                    failedEvents.push_back({{"id", m.id}, {"peer", m.peerKey}, {"status", "failed"}});
                    continue;
                }
                m.tries++;
                m.lastSendMs = now;
                resend.push_back({m.rawPacket, {m.peerAddr, m.peerPort}});
            }
            for (auto it = offers.begin(); it != offers.end();) {
                if (now - it->second.createdMs > kOfferTtlMs) it = offers.erase(it);
                else ++it;
            }
            for (auto it = seenPackets.begin(); it != seenPackets.end();) {
                if (now - it->second > 10 * 60 * 1000) it = seenPackets.erase(it);
                else ++it;
            }
            if (dirty && now - lastSaveMs > 1000) SaveLocked();
        }
        for (auto& r : resend) SendToKey(r.first, r.second.first, r.second.second);
        for (auto& e : failedEvents) LogEvent("status", e);
        ReapWorkers(false);
    }

    void UdpLoop() {
        std::vector<char> buf(kMaxUdpPacket + 1);
        while (!stop.load()) {
            int r = WaitSock(udp, 300);
            if (stop.load()) break;
            if (r > 0) {
                sockaddr_in from{};
#ifdef _WIN32
                int fromLen = sizeof(from);
#else
                socklen_t fromLen = sizeof(from);
#endif
                int n = ::recvfrom(udp, buf.data(), static_cast<int>(kMaxUdpPacket), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n > 0) {
                    uint32_t a = ntohl(from.sin_addr.s_addr);
                    if (IsAllowed(a)) {
                        try {
                            HandlePacket(std::string(buf.data(), static_cast<size_t>(n)), a, ntohs(from.sin_port));
                        } catch (const std::exception& e) {
                            WriteAuditLog("IPMSG_ERR", std::string("packet: ") + e.what());
                        }
                    }
                }
            }
            Maintenance();
        }
    }

    // --- File transfer (TCP) ---
    void StartWorker(std::function<void()> fn) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lk(workersMu);
        workers.push_back({std::thread([fn = std::move(fn), done]() {
            try { fn(); } catch (...) {}
            done->store(true);
        }), done});
    }

    void ReapWorkers(bool all) {
        std::vector<std::thread> toJoin;
        {
            std::lock_guard<std::mutex> lk(workersMu);
            for (auto it = workers.begin(); it != workers.end();) {
                if (all || it->done->load()) {
                    toJoin.push_back(std::move(it->th));
                    it = workers.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& t : toJoin) if (t.joinable()) t.join();
    }

    void ServeConnection(socket_t c, uint32_t peerAddr) {
        // Read the GETFILEDATA request (one packet, no terminator guaranteed).
        std::string req;
        char buf[2048];
        int64_t deadline = NowMs() + 10000;
        while (!stop.load() && NowMs() < deadline && req.size() < 4096) {
            int r = WaitSock(c, 200);
            if (r < 0) break;
            if (r == 0) { if (!req.empty()) break; continue; }
            int n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
            if (std::count(req.begin(), req.end(), ':') >= 7) {
                // Give the remaining bytes of the same segment a moment to land.
                if (WaitSock(c, 50) <= 0) break;
            }
        }
        size_t z = req.find('\0');
        if (z != std::string::npos) req.resize(z);
        std::vector<std::string> parts = SplitOn(req, ':');
        if (parts.size() < 8) { CloseSock(c); return; }
        uint64_t cmd = 0;
        ParseDec(parts[4], cmd);
        if (GetMode(static_cast<uint32_t>(cmd)) != IPMSG_GETFILEDATA) { CloseSock(c); return; }
        uint64_t pkt = 0, fid = 0, off = 0;
        if (!ParseHex(Trim(parts[5]), pkt) || !ParseHex(Trim(parts[6]), fid)) { CloseSock(c); return; }
        ParseHex(Trim(parts[7]), off);

        std::string path;
        int64_t size = 0;
        uint64_t msgId = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            auto it = offers.find(pkt);
            const std::string addr = Ipv4ToString(peerAddr);
            if (it != offers.end() &&
                (it->second.peerAddrs.empty() || it->second.peerAddrs.count(addr))) {
                for (const auto& a : it->second.files) {
                    if (a.fileId == fid) { path = a.localPath; size = a.size; break; }
                }
            }
            for (auto& m : messages) {
                if (m.direction == "out" && m.packetNo == pkt) { msgId = m.id; break; }
            }
        }
        if (path.empty()) { CloseSock(c); return; }
        FILE* f = OpenFileUtf8(path, "rb");
        if (!f) { CloseSock(c); return; }
        bool ok = true;
        if (off > 0 && FileSeek(f, static_cast<int64_t>(off)) != 0) ok = false;
        std::vector<char> data(256 * 1024);
        int64_t sent = static_cast<int64_t>(off);
        while (ok && !stop.load() && sent < size) {
            size_t want = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(data.size()), size - sent));
            size_t n = std::fread(data.data(), 1, want, f);
            if (n == 0) break;
            if (!SendAll(c, data.data(), n, stop)) { ok = false; break; }
            sent += static_cast<int64_t>(n);
        }
        std::fclose(f);
        CloseSock(c);
        if (ok && sent >= size && msgId) {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (Message* m = FindMessageLocked(msgId)) {
                    for (auto& a : m->files) if (a.fileId == fid) { a.status = "sent"; a.bytes = size; }
                    dirty = true;
                    Touch();
                }
            }
            LogEvent("file_sent", {{"id", msgId}, {"file_id", fid}, {"path", path}, {"bytes", size},
                                   {"peer", Ipv4ToString(peerAddr)}});
        }
    }

    void TcpLoop() {
        while (!stop.load()) {
            int r = WaitSock(tcp, 300);
            if (stop.load()) break;
            if (r <= 0) continue;
            sockaddr_in from{};
#ifdef _WIN32
            int fromLen = sizeof(from);
#else
            socklen_t fromLen = sizeof(from);
#endif
            socket_t c = ::accept(tcp, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (c == kInvalidSocket) continue;
            uint32_t a = ntohl(from.sin_addr.s_addr);
            if (!IsAllowed(a)) { CloseSock(c); continue; }
            StartWorker([this, c, a]() { ServeConnection(c, a); });
        }
    }

    std::string UniquePath(const std::string& dir, const std::string& name) {
        std::wstring wdir = Utf8ToWide(dir);
        std::wstring base = Utf8ToWide(name);
        std::wstring stem = base, ext;
        size_t dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) { stem = base.substr(0, dot); ext = base.substr(dot); }
        std::wstring candidate = PathJoinWide(wdir, base);
        for (int i = 1; PathExistsWide(candidate) && i < 10000; ++i) {
            candidate = PathJoinWide(wdir, stem + L" (" + std::to_wstring(i) + L")" + ext);
        }
        return WideToUtf8(candidate);
    }

    void DownloadOne(uint64_t msgId, uint64_t fileId, std::string dir) {
        std::string addr, name;
        uint16_t port = 0;
        uint64_t pkt = 0;
        int64_t size = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            Message* m = FindMessageLocked(msgId);
            if (!m) return;
            pkt = m->packetNo;
            addr = m->peerAddr;
            port = m->peerPort;
            for (auto& a : m->files) {
                if (a.fileId == fileId) { name = a.name; size = a.size; }
            }
        }
        auto finish = [&](const std::string& status, const std::string& path, int64_t bytes, const std::string& err) {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (Message* m = FindMessageLocked(msgId)) {
                    for (auto& a : m->files) {
                        if (a.fileId != fileId) continue;
                        a.status = status;
                        a.bytes = bytes;
                        if (!path.empty()) a.localPath = path;
                        a.error = err;
                    }
                    dirty = true;
                    Touch();
                }
            }
            nlohmann::json ev = {{"id", msgId}, {"file_id", fileId}, {"name", name}, {"status", status},
                                 {"bytes", bytes}, {"peer", addr + ":" + std::to_string(port)}};
            if (!path.empty()) ev["path"] = path;
            if (!err.empty()) ev["error"] = err;
            LogEvent("file_received", ev);
        };
        if (size > cfg.maxFileBytes) { finish("failed", "", 0, "file exceeds max_file_mb"); return; }
        EnsureDirectoryWide(Utf8ToWide(dir));
        std::string path = UniquePath(dir, SafeFileName(name));

        socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kInvalidSocket) { finish("failed", "", 0, "socket failed"); return; }
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        uint32_t a = 0;
        ParseIpv4(addr, a);
        to.sin_addr.s_addr = htonl(a);
        if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0) {
            CloseSock(s);
            finish("failed", "", 0, "connect failed (" + std::to_string(LastSockError()) + "); the sender may have released the file");
            return;
        }
        std::string req;
        {
            std::lock_guard<std::mutex> lk(mu);
            req = Header(NewPacketNo(), IPMSG_GETFILEDATA) + ToHex(pkt) + ":" + ToHex(fileId) + ":0:";
        }
        if (!SendAll(s, req.data(), req.size(), stop)) {
            CloseSock(s);
            finish("failed", "", 0, "request failed");
            return;
        }
        FILE* f = OpenFileUtf8(path, "wb");
        if (!f) { CloseSock(s); finish("failed", "", 0, "cannot create " + path); return; }
        std::vector<char> buf(256 * 1024);
        int64_t got = 0;
        int64_t lastReport = 0;
        bool ok = true;
        int idleMs = 0;
        while (got < size && !stop.load()) {
            int r = WaitSock(s, 500);
            if (r < 0) { ok = false; break; }
            if (r == 0) {
                idleMs += 500;
                if (idleMs > 30000) { ok = false; break; }
                continue;
            }
            idleMs = 0;
            int n = ::recv(s, buf.data(), static_cast<int>(std::min<int64_t>(static_cast<int64_t>(buf.size()), size - got)), 0);
            if (n <= 0) break;
            if (std::fwrite(buf.data(), 1, static_cast<size_t>(n), f) != static_cast<size_t>(n)) { ok = false; break; }
            got += n;
            if (got - lastReport > 4 * 1024 * 1024) {
                lastReport = got;
                std::lock_guard<std::mutex> lk(mu);
                if (Message* m = FindMessageLocked(msgId)) {
                    for (auto& at : m->files) if (at.fileId == fileId) at.bytes = got;
                    self->version_.fetch_add(1);
                }
            }
        }
        std::fclose(f);
        CloseSock(s);
        if (!ok || got < size) {
            RemoveFileWide(Utf8ToWide(path));
            finish("failed", "", got, stop.load() ? "stopped" : "transfer incomplete");
            return;
        }
        finish("done", path, got, "");
    }

    // fileId < 0: every available file of the message.
    nlohmann::json StartDownloads(uint64_t msgId, int64_t fileId, std::string dir) {
        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lk(mu);
            Message* m = FindMessageLocked(msgId);
            if (!m) return {{"ok", false}, {"error", "message not found: " + std::to_string(msgId)}};
            if (m->direction != "in") return {{"ok", false}, {"error", "message has no incoming files"}};
            if (dir.empty()) dir = DownloadDir();
            for (auto& a : m->files) {
                if (fileId >= 0 && a.fileId != static_cast<uint64_t>(fileId)) continue;
                if (a.status != "available" && a.status != "failed") continue;
                a.status = "downloading";
                a.bytes = 0;
                a.error.clear();
                ids.push_back(a.fileId);
            }
            if (ids.empty()) return {{"ok", false}, {"error", "no downloadable file (already done, expired or unsupported)"}};
            Touch();
        }
        for (uint64_t fid : ids) {
            StartWorker([this, msgId, fid, dir]() { DownloadOne(msgId, fid, dir); });
        }
        nlohmann::json out;
        out["ok"] = true;
        out["started"] = ids;
        out["dir"] = dir;
        return out;
    }
};

// ---------------------------------------------------------------------------

IpmsgNode::IpmsgNode() : impl_(std::make_unique<Impl>()) {
    impl_->self = this;
    impl_->nextPacketNo = static_cast<uint64_t>(std::time(nullptr));
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->Load();
}

IpmsgNode::~IpmsgNode() { Stop(); }

IpmsgConfig IpmsgNode::GetConfig() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->cfg;
}

bool IpmsgNode::Start(const IpmsgConfig& cfgIn, std::string* outError) {
    Stop();
    Impl& d = *impl_;
    EnsureNetworking();
    {
        std::lock_guard<std::mutex> lk(d.mu);
        d.cfg = cfgIn;
        d.allow.clear();
        for (const auto& s : cfgIn.allowFrom) {
            Cidr c;
            if (ParseCidr(s, c)) d.allow.push_back(c);
        }
        d.selfUser = LocalUserName();
        d.selfHost = LocalHostName();
        d.selfNick = cfgIn.nickname.empty() ? ("Hancho@" + d.selfHost) : cfgIn.nickname;
        d.peers.clear();
    }
    auto fail = [&](const std::string& msg) {
        if (d.udp != kInvalidSocket) { CloseSock(d.udp); d.udp = kInvalidSocket; }
        if (d.tcp != kInvalidSocket) { CloseSock(d.tcp); d.tcp = kInvalidSocket; }
        if (outError) *outError = msg;
        WriteAuditLog("IPMSG_START_FAILED", msg);
        return false;
    };
    uint32_t bindAddr = 0;
    if (!ParseIpv4(cfgIn.bind, bindAddr)) return fail("invalid bind address: " + cfgIn.bind);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(cfgIn.port);
    sa.sin_addr.s_addr = htonl(bindAddr);

    d.udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (d.udp == kInvalidSocket) return fail("cannot create UDP socket");
    int on = 1;
    setsockopt(d.udp, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
#ifdef _WIN32
    // Do not let another process steal the port.
    BOOL excl = TRUE;
    setsockopt(d.udp, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
#endif
    if (::bind(d.udp, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0)
        return fail("cannot bind UDP " + cfgIn.bind + ":" + std::to_string(cfgIn.port) +
                    " (error " + std::to_string(LastSockError()) + ")");

    d.tcp = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (d.tcp == kInvalidSocket) return fail("cannot create TCP socket");
#ifdef _WIN32
    setsockopt(d.tcp, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
#else
    setsockopt(d.tcp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
#endif
    if (::bind(d.tcp, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || ::listen(d.tcp, 16) != 0)
        return fail("cannot listen on TCP " + cfgIn.bind + ":" + std::to_string(cfgIn.port) +
                    " (error " + std::to_string(LastSockError()) + ")");

    d.stop = false;
    running_ = true;
    d.udpThread = std::thread([&d]() { d.UdpLoop(); });
    d.tcpThread = std::thread([&d]() { d.TcpLoop(); });
    std::string entry;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        entry = d.EntryPacket(IPMSG_BR_ENTRY);
        d.Touch();
    }
    d.SendEntryTargets(entry);
    WriteAuditLog("IPMSG_START", "listening on " + cfgIn.bind + ":" + std::to_string(cfgIn.port) +
                                     " (UDP+TCP) as " + d.selfNick);
    return true;
}

void IpmsgNode::Stop() {
    Impl& d = *impl_;
    if (!running_.exchange(false)) return;
    std::string bye;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        bye = d.Header(d.NewPacketNo(), IPMSG_BR_EXIT | IPMSG_CAPUTF8OPT) + Utf8ToCp932(d.selfNick);
        bye.push_back('\0');
        bye += Utf8ToCp932(d.cfg.group);
    }
    d.SendEntryTargets(bye);
    d.stop = true;
    d.cv.notify_all();
    if (d.udpThread.joinable()) d.udpThread.join();
    if (d.tcpThread.joinable()) d.tcpThread.join();
    d.ReapWorkers(true);
    CloseSock(d.udp);
    d.udp = kInvalidSocket;
    CloseSock(d.tcp);
    d.tcp = kInvalidSocket;
    std::lock_guard<std::mutex> lk(d.mu);
    d.peers.clear();
    d.offers.clear();
    for (auto& m : d.messages) {
        if (m.status == "sending") { m.status = "failed"; d.dirty = true; }
    }
    if (d.dirty) d.SaveLocked();
    d.Touch();
    WriteAuditLog("IPMSG_STOP", "stopped");
}

nlohmann::json IpmsgNode::Status() const {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lk(d.mu);
    nlohmann::json out;
    out["ok"] = true;
    out["running"] = running_.load();
    out["config"] = d.cfg.ToJson();
    out["user"] = d.selfUser;
    out["host"] = d.selfHost;
    out["nickname"] = d.selfNick;
    out["peer_count"] = d.peers.size();
    size_t unread = 0;
    uint64_t lastId = 0;
    for (const auto& m : d.messages) {
        if (m.direction != "out" && !m.read) ++unread;
        lastId = std::max(lastId, m.id);
    }
    out["unread"] = unread;
    out["last_id"] = lastId;
    out["message_count"] = d.messages.size();
    out["download_dir"] = d.DownloadDir();
    out["version"] = version_.load();
    return out;
}

nlohmann::json IpmsgNode::Peers() const {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lk(d.mu);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& kv : d.peers) {
        const auto& p = kv.second;
        nlohmann::json j;
        j["key"] = p.key;
        j["addr"] = p.addr;
        j["port"] = p.port;
        j["user"] = p.user;
        j["host"] = p.host;
        j["nickname"] = p.nickname;
        j["group"] = p.group;
        j["absent"] = p.absent;
        j["hancho"] = p.hancho;
        j["utf8"] = p.capUtf8;
        j["last_seen"] = FormatLocalTime(p.lastSeenMs);
        if (!p.info.empty()) j["info"] = p.info;
        arr.push_back(j);
    }
    nlohmann::json out;
    out["ok"] = true;
    out["running"] = running_.load();
    out["peers"] = arr;
    return out;
}

nlohmann::json IpmsgNode::Refresh() {
    Impl& d = *impl_;
    if (!running_.load()) return {{"ok", false}, {"error", "IPMsg is disabled (set ipmsg.enabled=true)"}};
    std::string entry;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        entry = d.EntryPacket(IPMSG_BR_ENTRY);
    }
    d.SendEntryTargets(entry);
    return {{"ok", true}, {"sent", true}, {"hint", "Peers answer within a second; call ipmsg_peers."}};
}

nlohmann::json IpmsgNode::Send(const std::string& toIn, const std::string& text,
                               const std::string& fromAgent, const std::string& toAgent,
                               uint64_t replyTo, const std::vector<std::string>& files,
                               const std::string& audienceIn, int hops) {
    Impl& d = *impl_;
    if (!running_.load()) return {{"ok", false}, {"error", "IPMsg is disabled (set ipmsg.enabled=true)"}};
    if (text.empty() && files.empty()) return {{"ok", false}, {"error", "text or files required"}};
    const std::string to = Trim(toIn);
    std::string audience = audienceIn;
    if (audience == "llm" || audience == "hancho" || audience == "internal") audience = "agent";
    if (!audience.empty() && audience != "human" && audience != "agent")
        return {{"ok", false}, {"error", "audience must be human, agent or empty"}};
    if (hops < 0) hops = 0;

    // Local delivery between agents on this node.
    if (to.empty() || to == "local" || to == "self") {
        if (!files.empty()) return {{"ok", false}, {"error", "files cannot be sent to local agents; share the path in the text"}};
        std::unique_lock<std::mutex> lk(d.mu);
        Impl::Message m;
        m.direction = "local";
        m.peerKey = "local";
        m.peerUser = d.selfUser;
        m.peerHost = d.selfHost;
        m.peerNick = d.selfNick;
        m.text = text;
        m.fromAgent = fromAgent;
        m.toAgent = toAgent;
        m.replyTo = replyTo;
        m.status = "delivered";
        m.audience = audience;
        m.hops = hops;
        auto& stored = d.AddMessageLocked(std::move(m));
        nlohmann::json out;
        out["ok"] = true;
        out["ids"] = {stored.id};
        out["local"] = true;
        nlohmann::json mj = d.MessageJson(stored);
        lk.unlock();
        d.LogEvent("local", mj);
        d.Notify(mj);
        return out;
    }

    // Validate files first.
    std::vector<Impl::Attachment> atts;
    uint64_t fid = 0;
    for (const auto& fIn : files) {
        std::string f = ResolveAbsolutePathUtf8(fIn);
        std::wstring wf = Utf8ToWide(f);
        if (!PathExistsWide(wf) || IsDirectoryWide(wf)) return {{"ok", false}, {"error", "not a regular file: " + fIn}};
        Impl::Attachment a;
        a.fileId = fid++;
        a.localPath = f;
        std::string name = f;
        size_t sl = name.find_last_of("/\\");
        if (sl != std::string::npos) name = name.substr(sl + 1);
        a.name = name;
        std::error_code ec;
        a.size = static_cast<int64_t>(std::filesystem::file_size(std::filesystem::path(wf), ec));
        if (ec) return {{"ok", false}, {"error", "cannot stat: " + fIn}};
        uint64_t ft = 0;
        if (GetFileMtimeWide(wf, ft) && ft > 116444736000000000ull)
            a.mtime = static_cast<int64_t>((ft - 116444736000000000ull) / 10000000ull);
        a.status = "offered";
        atts.push_back(std::move(a));
    }

    std::vector<Impl::Peer> targets;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        if (to == "*" || to == "all") {
            for (const auto& kv : d.peers) targets.push_back(kv.second);
            if (targets.empty()) return {{"ok", false}, {"error", "no peers known; run ipmsg_refresh first"}};
        } else {
            auto lower = [](std::string s) {
                for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            const std::string lt = lower(to);
            for (const auto& kv : d.peers) {
                const auto& p = kv.second;
                if (p.key == to || p.addr == to || lower(p.nickname) == lt ||
                    lower(p.user + "@" + p.host) == lt || lower(p.host) == lt) {
                    targets.push_back(p);
                }
            }
            if (targets.size() > 1) {
                // Prefer an exact key / address match when a name is ambiguous.
                std::vector<Impl::Peer> exact;
                for (auto& p : targets) if (p.key == to) exact.push_back(p);
                if (exact.size() == 1) targets = exact;
                else {
                    nlohmann::json keys = nlohmann::json::array();
                    for (auto& p : targets) keys.push_back(p.key);
                    return {{"ok", false}, {"error", "ambiguous peer '" + to + "'; use one of the keys"}, {"matches", keys}};
                }
            }
            if (targets.empty()) {
                // Unknown host: allow "ip" or "ip:port" directly.
                std::string addr = to;
                uint16_t port = d.cfg.port;
                size_t colon = to.rfind(':');
                if (colon != std::string::npos) {
                    addr = to.substr(0, colon);
                    try {
                        int pn = std::stoi(to.substr(colon + 1));
                        if (pn > 0 && pn < 65536) port = static_cast<uint16_t>(pn);
                    } catch (...) {}
                }
                uint32_t a = 0;
                if (!ParseIpv4(addr, a)) return {{"ok", false}, {"error", "unknown peer: " + to + " (see ipmsg_peers)"}};
                if (!d.IsAllowed(a)) return {{"ok", false}, {"error", "address not allowed (LAN only / allow_from): " + addr}};
                Impl::Peer p;
                p.addr = addr;
                p.port = port;
                p.key = addr + ":" + std::to_string(port);
                p.nickname = p.key;
                p.capUtf8 = true;
                targets.push_back(p);
            }
        }
    }

    const bool multi = targets.size() > 1;
    nlohmann::json ids = nlohmann::json::array();
    std::vector<std::pair<std::string, Impl::Peer>> toSend;
    std::vector<nlohmann::json> sentJson;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        const uint64_t pkt = d.NewPacketNo();
        for (const auto& p : targets) {
            uint32_t cmd = IPMSG_SENDMSG | IPMSG_SENDCHECKOPT;
            if (multi) cmd |= IPMSG_MULTICASTOPT;
            if (p.capUtf8) cmd |= IPMSG_UTF8OPT | IPMSG_CAPUTF8OPT;
            if (!atts.empty()) cmd |= IPMSG_FILEATTACHOPT;
            auto enc = [&](const std::string& s) { return p.capUtf8 ? s : Utf8ToCp932(s); };
            std::string wireText;
            for (char c : text) {
                if (c == '\n') wireText += "\r\n";
                else if (c != '\r') wireText.push_back(c);
            }
            std::string packet = d.Header(pkt, cmd) + enc(wireText);
            packet.push_back('\0');
            for (const auto& a : atts) {
                std::string nm;
                for (char c : enc(a.name)) { nm.push_back(c); if (c == ':') nm.push_back(':'); }
                packet += ToHex(a.fileId) + ":" + nm + ":" + ToHex(static_cast<uint64_t>(a.size)) + ":" +
                          ToHex(static_cast<uint64_t>(a.mtime)) + ":" + ToHex(IPMSG_FILE_REGULAR) + ":\a";
            }
            packet.push_back('\0');
            nlohmann::json ext = nlohmann::json::object();
            if (!fromAgent.empty()) ext["from_agent"] = fromAgent;
            if (!toAgent.empty()) ext["to_agent"] = toAgent;
            if (replyTo) ext["reply_to"] = replyTo;
            if (!audience.empty()) ext["audience"] = audience;
            if (hops) ext["hops"] = hops;
            ext["node"] = d.selfNick;
            packet += kHanchoExtTag + ext.dump();
            if (packet.size() > kMaxUdpPacket) return {{"ok", false}, {"error", "message too long for one IPMsg packet"}};

            Impl::Message m;
            m.direction = "out";
            m.peerKey = p.key;
            m.peerAddr = p.addr;
            m.peerPort = p.port;
            m.peerUser = p.user;
            m.peerHost = p.host;
            m.peerNick = p.nickname;
            m.text = text;
            m.fromAgent = fromAgent;
            m.toAgent = toAgent;
            m.replyTo = replyTo;
            m.packetNo = pkt;
            m.broadcast = multi;
            m.read = true;
            m.status = "sending";
            m.audience = audience;
            m.hops = hops;
            m.files = atts;
            m.rawPacket = packet;
            m.tries = 1;
            m.lastSendMs = NowMs();
            auto& stored = d.AddMessageLocked(std::move(m));
            ids.push_back(stored.id);
            sentJson.push_back(d.MessageJson(stored));
            toSend.push_back({packet, p});
        }
        if (!atts.empty()) {
            Impl::Offer off;
            off.packetNo = pkt;
            for (const auto& p : targets) off.peerAddrs.insert(p.addr);
            off.files = atts;
            off.createdMs = NowMs();
            d.offers[pkt] = std::move(off);
        }
    }
    for (auto& s : toSend) d.SendToKey(s.first, s.second.addr, s.second.port);
    for (auto& j : sentJson) d.LogEvent("send", j);
    nlohmann::json out;
    out["ok"] = true;
    out["ids"] = ids;
    out["status"] = "sending";
    out["hint"] = "Delivery is confirmed asynchronously; status becomes delivered or failed (see ipmsg_inbox include_outgoing).";
    return out;
}

static bool MatchesAgent(const std::string& toAgent, const std::string& agent, bool includeUnaddressed) {
    if (agent.empty()) return true;
    if (toAgent.empty()) return includeUnaddressed;
    return toAgent == agent || toAgent == "*" || toAgent == "all";
}

nlohmann::json IpmsgNode::Inbox(uint64_t sinceId, const std::string& agent, bool includeUnaddressed,
                                bool unreadOnly, bool includeOutgoing, size_t limit, bool markRead) {
    Impl& d = *impl_;
    std::vector<std::pair<std::string, std::pair<std::string, uint16_t>>> receipts;
    nlohmann::json out;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        std::vector<Impl::Message*> hits;
        for (auto& m : d.messages) {
            if (m.id <= sinceId) continue;
            if (m.direction == "out" && !includeOutgoing) continue;
            if (m.direction != "out" && !MatchesAgent(m.toAgent, agent, includeUnaddressed)) continue;
            if (unreadOnly && m.read) continue;
            hits.push_back(&m);
        }
        if (limit > 0 && hits.size() > limit) {
            // Oldest first when paging forward from a cursor; newest otherwise.
            if (sinceId > 0) hits.resize(limit);
            else hits.erase(hits.begin(), hits.end() - static_cast<std::ptrdiff_t>(limit));
        }
        nlohmann::json arr = nlohmann::json::array();
        uint64_t lastId = sinceId;
        bool changed = false;
        for (auto* m : hits) {
            arr.push_back(d.MessageJson(*m));
            lastId = std::max(lastId, m->id);
            if (markRead && !m->read && m->direction != "out") {
                m->read = true;
                changed = true;
                if (m->readCheck && d.udp != kInvalidSocket) {
                    receipts.push_back({d.Header(d.NewPacketNo(), IPMSG_READMSG) + std::to_string(m->packetNo),
                                        {m->peerAddr, m->peerPort}});
                }
            }
        }
        if (changed) { d.dirty = true; d.Touch(); }
        out["ok"] = true;
        out["running"] = running_.load();
        out["messages"] = arr;
        out["last_id"] = lastId;
    }
    for (auto& r : receipts) d.SendToKey(r.first, r.second.first, r.second.second);
    return out;
}

nlohmann::json IpmsgNode::Wait(uint64_t sinceId, const std::string& agent, bool includeUnaddressed,
                               int timeoutMs, bool markRead) {
    Impl& d = *impl_;
    if (timeoutMs < 0) timeoutMs = 0;
    if (timeoutMs > 600000) timeoutMs = 600000;
    {
        std::unique_lock<std::mutex> lk(d.mu);
        auto has = [&]() {
            for (const auto& m : d.messages) {
                if (m.id > sinceId && m.direction != "out" && MatchesAgent(m.toAgent, agent, includeUnaddressed))
                    return true;
            }
            return false;
        };
        d.cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]() { return has() || d.stop.load(); });
        if (!has()) {
            uint64_t lastId = sinceId;
            nlohmann::json out;
            out["ok"] = true;
            out["timed_out"] = true;
            out["messages"] = nlohmann::json::array();
            out["last_id"] = lastId;
            out["running"] = running_.load();
            out["hint"] = "No new message; call ipmsg_wait again with the same since_id.";
            return out;
        }
    }
    nlohmann::json out = Inbox(sinceId, agent, includeUnaddressed, false, false, 50, markRead);
    out["timed_out"] = false;
    return out;
}

nlohmann::json IpmsgNode::MarkRead(const std::vector<uint64_t>& ids, bool all) {
    Impl& d = *impl_;
    std::vector<std::pair<std::string, std::pair<std::string, uint16_t>>> receipts;
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        std::set<uint64_t> want(ids.begin(), ids.end());
        for (auto& m : d.messages) {
            if (m.direction == "out" || m.read) continue;
            if (!all && !want.count(m.id)) continue;
            m.read = true;
            ++n;
            if (m.readCheck && d.udp != kInvalidSocket) {
                receipts.push_back({d.Header(d.NewPacketNo(), IPMSG_READMSG) + std::to_string(m.packetNo),
                                    {m.peerAddr, m.peerPort}});
            }
        }
        if (n) { d.dirty = true; d.Touch(); }
    }
    for (auto& r : receipts) d.SendToKey(r.first, r.second.first, r.second.second);
    return {{"ok", true}, {"marked", n}};
}

nlohmann::json IpmsgNode::Download(uint64_t messageId, int64_t fileId, const std::string& dir,
                                   bool wait, int timeoutMs) {
    Impl& d = *impl_;
    if (!running_.load()) return {{"ok", false}, {"error", "IPMsg is disabled (set ipmsg.enabled=true)"}};
    std::string target = dir.empty() ? std::string() : ResolveAbsolutePathUtf8(dir);
    nlohmann::json started = d.StartDownloads(messageId, fileId, target);
    if (!started.value("ok", false) || !wait) return started;
    if (timeoutMs <= 0) timeoutMs = 300000;
    if (timeoutMs > 3600000) timeoutMs = 3600000;
    std::unique_lock<std::mutex> lk(d.mu);
    auto finished = [&]() {
        Impl::Message* m = d.FindMessageLocked(messageId);
        if (!m) return true;
        for (const auto& a : m->files) if (a.status == "downloading") return false;
        return true;
    };
    d.cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]() { return finished() || d.stop.load(); });
    nlohmann::json out = started;
    out["finished"] = finished();
    if (Impl::Message* m = d.FindMessageLocked(messageId)) out["message"] = d.MessageJson(*m);
    return out;
}

nlohmann::json IpmsgNode::Get(uint64_t id) const {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lk(d.mu);
    for (const auto& m : d.messages) {
        if (m.id == id) return {{"ok", true}, {"message", d.MessageJson(m)}};
    }
    return {{"ok", false}, {"error", "message not found: " + std::to_string(id)}};
}

void IpmsgNode::SetOnMessage(std::function<void(const nlohmann::json&)> cb) {
    std::lock_guard<std::mutex> lk(impl_->cbMu);
    impl_->onMessage = std::move(cb);
}

void IpmsgNode::LogEvent(const std::string& event, const nlohmann::json& body) {
    impl_->LogEvent(event, body);
}

nlohmann::json IpmsgNode::Log(const std::string& since, const std::string& until,
                              const std::string& query, const std::string& peer,
                              const std::string& agent, const std::string& event,
                              size_t limit) const {
    Impl& d = *impl_;
    // Day range: "YYYY-MM-DD" bounds, "today", or a duration ("7d", "24h") for since.
    auto dayOf = [](int64_t ms) { return FormatLocalTime(ms).substr(0, 10); };
    const int64_t now = NowMs();
    std::string fromDay = dayOf(now - 7ll * 86400 * 1000);
    std::string toDay = dayOf(now);
    if (!since.empty()) {
        int64_t sec = 0;
        if (since == "today") fromDay = dayOf(now);
        else if (ParseDurationToSec(since, sec)) fromDay = dayOf(now - sec * 1000);
        else fromDay = since.substr(0, 10);
    }
    if (!until.empty()) toDay = (until == "today") ? dayOf(now) : until.substr(0, 10);
    if (limit == 0) limit = 200;
    if (limit > 5000) limit = 5000;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string q = lower(query);
    const std::string lpeer = lower(peer);
    std::vector<std::wstring> files = ListDirectoryWide(d.LogDir(), L"ipmsg-*.jsonl");
    std::vector<std::string> names;
    for (const auto& f : files) {
        std::string n = WideToUtf8(f);
        size_t sl = n.find_last_of("/\\");
        if (sl != std::string::npos) n = n.substr(sl + 1);
        if (n.size() < 16 || n.rfind("ipmsg-", 0) != 0) continue;
        std::string day = n.substr(6, 10);
        if (day < fromDay || day > toDay) continue;
        names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    std::deque<nlohmann::json> hits;
    size_t total = 0;
    for (const auto& n : names) {
        std::string content;
        {
            std::lock_guard<std::mutex> lk(d.logMu);
            content = ReadFileUtf8Wide(PathJoinWide(d.LogDir(), Utf8ToWide(n)));
        }
        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (!q.empty() && lower(line).find(q) == std::string::npos) continue;
            nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
            if (!j.is_object()) continue;
            if (!event.empty() && j.value("event", std::string()) != event) continue;
            if (!lpeer.empty()) {
                std::string pk;
                if (j.contains("peer") && j["peer"].is_object()) {
                    const auto& p = j["peer"];
                    pk = p.value("key", std::string()) + " " + p.value("nickname", std::string()) + " " +
                         p.value("host", std::string());
                } else if (j.contains("peer") && j["peer"].is_string()) {
                    pk = j["peer"].get<std::string>();
                }
                if (lower(pk).find(lpeer) == std::string::npos) continue;
            }
            if (!agent.empty() && j.value("from_agent", std::string()) != agent &&
                j.value("to_agent", std::string()) != agent && j.value("agent", std::string()) != agent)
                continue;
            ++total;
            hits.push_back(std::move(j));
            if (hits.size() > limit) hits.pop_front();
        }
    }
    nlohmann::json out;
    out["ok"] = true;
    out["entries"] = nlohmann::json::array();
    for (auto& h : hits) out["entries"].push_back(std::move(h));
    out["matched"] = total;
    out["truncated"] = total > out["entries"].size();
    out["from"] = fromDay;
    out["to"] = toDay;
    out["log_dir"] = WideToUtf8(d.LogDir());
    return out;
}

nlohmann::json IpmsgNode::Clear() {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lk(d.mu);
    size_t n = d.messages.size();
    d.messages.clear();
    d.SaveLocked();
    d.Touch();
    return {{"ok", true}, {"cleared", n}};
}

} // namespace pc
