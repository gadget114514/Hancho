#include "remote_client.h"

#include "common.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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
namespace remote {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
const socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
const socket_t kInvalidSocket = -1;
#endif

constexpr size_t kMaxMessageBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxHeaderBytes = 32u * 1024u;

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ContainsNoCase(const std::string& haystack, const std::string& needle) {
    return LowerAscii(haystack).find(LowerAscii(needle)) != std::string::npos;
}

void EnsureNetworking() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data = {};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

void CloseSocket(socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

int LastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string SocketErrorText(int code) {
#ifdef _WIN32
    return "socket error " + std::to_string(code);
#else
    return std::string(std::strerror(code));
#endif
}

bool SetBlocking(socket_t s, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    if (blocking) flags &= ~O_NONBLOCK;
    else flags |= O_NONBLOCK;
    return fcntl(s, F_SETFL, flags) == 0;
#endif
}

void SetTimeouts(socket_t s, int timeoutMs) {
    if (timeoutMs <= 0) timeoutMs = 15000;
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval tv = {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool WaitWritable(socket_t s, int timeoutMs) {
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(s, &writeSet);
    timeval tv = {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
    int rc = select(0, nullptr, &writeSet, nullptr, &tv);
#else
    int rc = select(s + 1, nullptr, &writeSet, nullptr, &tv);
#endif
    return rc > 0;
}

socket_t ConnectTcp(const std::string& host, uint16_t port, int timeoutMs, std::string& outError) {
    EnsureNetworking();
    outError.clear();

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string portText = std::to_string(port);
    addrinfo* result = nullptr;
    int gai = getaddrinfo(host.c_str(), portText.c_str(), &hints, &result);
    if (gai != 0 || !result) {
        outError = "cannot resolve " + host + ": " + std::to_string(gai);
        return kInvalidSocket;
    }

    socket_t connected = kInvalidSocket;
    for (addrinfo* ai = result; ai && connected == kInvalidSocket; ai = ai->ai_next) {
        socket_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kInvalidSocket) continue;
        SetBlocking(s, false);
        int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool ok = false;
        if (rc == 0) {
            ok = true;
        } else {
            int err = LastSocketError();
#ifdef _WIN32
            const bool inProgress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
            const bool inProgress = (err == EINPROGRESS || err == EWOULDBLOCK);
#endif
            if (inProgress && WaitWritable(s, timeoutMs)) {
                int soError = 0;
                socklen_t len = sizeof(soError);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len) == 0 &&
                    soError == 0) {
                    ok = true;
                }
            }
        }
        if (ok) {
            SetBlocking(s, true);
            SetTimeouts(s, timeoutMs);
            connected = s;
        } else {
            CloseSocket(s);
        }
    }
    freeaddrinfo(result);
    if (connected == kInvalidSocket) {
        outError = "cannot connect to " + host + ":" + portText;
    }
    return connected;
}

bool SendAll(socket_t s, const void* data, size_t length, std::string& outError) {
    const char* p = static_cast<const char*>(data);
    size_t left = length;
    while (left > 0) {
#ifdef _WIN32
        int n = ::send(s, p, static_cast<int>(left), 0);
#else
        ssize_t n = ::send(s, p, left, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            outError = "send failed: " + SocketErrorText(LastSocketError());
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

struct SockBuf {
    socket_t s = kInvalidSocket;
    std::string buffer;
    int timeoutMs = 15000;

    bool Fill(std::string& outError) {
        char chunk[16384];
        int n = ::recv(s, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (n <= 0) {
            outError = n == 0 ? "connection closed by peer"
                              : "recv failed: " + SocketErrorText(LastSocketError());
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(n));
        return true;
    }

    bool ReadExact(size_t n, std::string& out, std::string& outError) {
        while (buffer.size() < n) {
            if (!Fill(outError)) return false;
            if (buffer.size() > kMaxMessageBytes) {
                outError = "message too large";
                return false;
            }
        }
        out.assign(buffer.data(), n);
        buffer.erase(0, n);
        return true;
    }

    bool ReadUntil(const std::string& delimiter, std::string& out, std::string& outError) {
        while (true) {
            size_t pos = buffer.find(delimiter);
            if (pos != std::string::npos) {
                out.assign(buffer.data(), pos + delimiter.size());
                buffer.erase(0, pos + delimiter.size());
                return true;
            }
            if (buffer.size() > kMaxHeaderBytes) {
                outError = "header too large";
                return false;
            }
            if (!Fill(outError)) return false;
        }
    }

    bool ReadToEofOrTimeout(std::string& out, std::string& outError) {
        out.clear();
        if (!buffer.empty()) {
            out.swap(buffer);
        }
        while (true) {
            if (!Fill(outError)) {
                if (outError == "connection closed by peer") {
                    outError.clear();
                    return true;
                }
                // A socket timeout is fine: use what the peer already sent.
                if (outError.find("recv failed") == 0) {
                    outError.clear();
                    return true;
                }
                return false;
            }
            out.append(buffer);
            buffer.clear();
            if (out.size() > kMaxMessageBytes) {
                outError = "message too large";
                return false;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// SHA-1 and base64 (needed for the WebSocket handshake)
// ---------------------------------------------------------------------------

inline uint32_t Rotl(uint32_t v, int bits) {
    return (v << bits) | (v >> (32 - bits));
}

class Sha1 {
public:
    Sha1() {
        h_[0] = 0x67452301u;
        h_[1] = 0xEFCDAB89u;
        h_[2] = 0x98BADCFEu;
        h_[3] = 0x10325476u;
        h_[4] = 0xC3D2E1F0u;
    }

    void Update(const void* data, size_t length) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total_ += length;
        while (length > 0) {
            size_t take = std::min(length, sizeof(block_) - blockLen_);
            std::memcpy(block_ + blockLen_, p, take);
            blockLen_ += take;
            p += take;
            length -= take;
            if (blockLen_ == sizeof(block_)) {
                Transform(block_);
                blockLen_ = 0;
            }
        }
    }

    void Final(std::array<uint8_t, 20>& out) {
        uint64_t bitLen = total_ * 8;
        uint8_t pad = 0x80;
        Update(&pad, 1);
        uint8_t zero = 0;
        while (blockLen_ != 56) Update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<uint8_t>(bitLen >> (56 - i * 8));
        Update(lenBytes, sizeof(lenBytes));
        for (int i = 0; i < 5; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>(h_[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
        }
    }

private:
    void Transform(const uint8_t* block) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = Rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rotl(b, 30);
            b = a;
            a = temp;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    uint32_t h_[5];
    uint8_t block_[64] = {};
    size_t blockLen_ = 0;
    uint64_t total_ = 0;
};

std::string Base64Encode(const uint8_t* data, size_t length) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((length + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= length) {
        uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out.push_back(table[(v >> 18) & 0x3F]);
        out.push_back(table[(v >> 12) & 0x3F]);
        out.push_back(table[(v >> 6) & 0x3F]);
        out.push_back(table[v & 0x3F]);
        i += 3;
    }
    if (i < length) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < length) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(table[(v >> 18) & 0x3F]);
        out.push_back(table[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < length ? table[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::string Base64Encode(const std::string& text) {
    return Base64Encode(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::string Sha1Base64(const std::string& text) {
    Sha1 sha;
    sha.Update(text.data(), text.size());
    std::array<uint8_t, 20> digest = {};
    sha.Final(digest);
    return Base64Encode(digest.data(), digest.size());
}

std::string RandomWebSocketKey() {
    std::array<uint8_t, 16> bytes = {};
    std::random_device rd;
    for (auto& b : bytes) b = static_cast<uint8_t>(rd() & 0xFF);
    return Base64Encode(bytes.data(), bytes.size());
}

std::string WebSocketAcceptFor(const std::string& key) {
    return Sha1Base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
}

std::string EncodeFrame(const std::string& payload, uint8_t opcode, bool mask) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | opcode));
    const size_t len = payload.size();
    uint8_t maskBit = mask ? 0x80 : 0x00;
    if (len <= 125) {
        out.push_back(static_cast<char>(maskBit | len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(maskBit | 126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(maskBit | 127));
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
    }
    if (mask) {
        uint8_t key[4];
        std::random_device rd;
        for (auto& b : key) b = static_cast<uint8_t>(rd() & 0xFF);
        for (uint8_t b : key) out.push_back(static_cast<char>(b));
        for (size_t i = 0; i < payload.size(); ++i) {
            out.push_back(static_cast<char>(payload[i] ^ key[i % 4]));
        }
    } else {
        out += payload;
    }
    return out;
}

// Reads one complete text message, answering ping frames on the way.
bool ReadWebSocketMessage(SockBuf& reader, bool clientRole, std::string& out, std::string& outError,
                          std::mutex* sendMutex = nullptr) {
    out.clear();
    bool fin = false;
    while (!fin) {
        std::string header;
        if (!reader.ReadExact(2, header, outError)) return false;
        const uint8_t b0 = static_cast<uint8_t>(header[0]);
        const uint8_t b1 = static_cast<uint8_t>(header[1]);
        fin = (b0 & 0x80) != 0;
        const uint8_t opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t len = b1 & 0x7F;
        if (len == 126) {
            std::string ext;
            if (!reader.ReadExact(2, ext, outError)) return false;
            len = (static_cast<uint8_t>(ext[0]) << 8) | static_cast<uint8_t>(ext[1]);
        } else if (len == 127) {
            std::string ext;
            if (!reader.ReadExact(8, ext, outError)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(ext[i]);
        }
        if (len > kMaxMessageBytes) {
            outError = "WebSocket frame too large";
            return false;
        }
        uint8_t maskKey[4] = {};
        if (masked) {
            std::string key;
            if (!reader.ReadExact(4, key, outError)) return false;
            for (int i = 0; i < 4; ++i) maskKey[i] = static_cast<uint8_t>(key[i]);
        }
        std::string payload;
        if (!reader.ReadExact(static_cast<size_t>(len), payload, outError)) return false;
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(payload[i] ^ maskKey[i % 4]);
            }
        }

        if (opcode == 0x8) {
            outError = "peer closed the WebSocket";
            return false;
        }
        if (opcode == 0x9) {
            std::string pong = EncodeFrame(payload, 0xA, clientRole);
            if (sendMutex) {
                std::lock_guard<std::mutex> lock(*sendMutex);
                if (!SendAll(reader.s, pong.data(), pong.size(), outError)) return false;
            } else {
                if (!SendAll(reader.s, pong.data(), pong.size(), outError)) return false;
            }
            continue;
        }
        if (opcode == 0xA) continue;
        if (opcode == 0x1 || opcode == 0x0) {
            out += payload;
            if (out.size() > kMaxMessageBytes) {
                outError = "WebSocket message too large";
                return false;
            }
        }
    }
    return true;
}

bool IsLoopbackAddress(const sockaddr_storage& addr) {
    if (addr.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        const uint32_t ip = ntohl(in->sin_addr.s_addr);
        return (ip >> 24) == 127;
    }
    if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        static const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        if (std::memcmp(&in6->sin6_addr, loopback, 16) == 0) return true;
        // ::ffff:127.x.x.x
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&in6->sin6_addr);
        bool mapped = true;
        for (int i = 0; i < 10; ++i) mapped = mapped && b[i] == 0;
        mapped = mapped && b[10] == 0xFF && b[11] == 0xFF && b[12] == 127;
        return mapped;
    }
    return false;
}

std::string PeerAddressText(const sockaddr_storage& addr) {
    char host[INET6_ADDRSTRLEN] = {};
    uint16_t port = 0;
    if (addr.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
        port = ntohs(in->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        port = ntohs(in6->sin6_port);
    }
    return std::string(host) + ":" + std::to_string(port);
}

std::string JsonError(const std::string& message) {
    nlohmann::json out;
    out["ok"] = false;
    out["error"] = message;
    return out.dump();
}

std::string RequestToken(const std::string& requestJson) {
    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    if (!req.is_object()) return "";
    auto it = req.find("token");
    if (it != req.end() && it->is_string()) return it->get<std::string>();
    return "";
}

struct PendingRequest {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    std::string response;
    std::string error;
};

std::string ExtractReqId(const nlohmann::json& doc) {
    if (!doc.is_object()) return "";
    if (doc.contains("req_id")) {
        return doc["req_id"].is_string() ? doc["req_id"].get<std::string>() : doc["req_id"].dump();
    }
    if (doc.contains("requestId")) {
        return doc["requestId"].is_string() ? doc["requestId"].get<std::string>() : doc["requestId"].dump();
    }
    if (doc.contains("request_id")) {
        return doc["request_id"].is_string() ? doc["request_id"].get<std::string>() : doc["request_id"].dump();
    }
    if (doc.contains("id")) {
        return doc["id"].is_string() ? doc["id"].get<std::string>() : doc["id"].dump();
    }
    return "";
}

std::string AttachRequestId(const std::string& respJson, const nlohmann::json& reqDoc) {
    if (!reqDoc.is_object()) return respJson;

    const bool hasReqId = reqDoc.contains("req_id");
    const bool hasRequestId = reqDoc.contains("requestId");
    const bool hasRequest_id = reqDoc.contains("request_id");
    const bool hasId = reqDoc.contains("id");

    if (!hasReqId && !hasRequestId && !hasRequest_id && !hasId) {
        return respJson;
    }

    nlohmann::json resp = nlohmann::json::parse(respJson, nullptr, false);
    if (!resp.is_object()) return respJson;

    if (hasReqId && !resp.contains("req_id")) {
        resp["req_id"] = reqDoc["req_id"];
    }
    if (hasRequestId && !resp.contains("requestId")) {
        resp["requestId"] = reqDoc["requestId"];
    }
    if (hasRequest_id && !resp.contains("request_id")) {
        resp["request_id"] = reqDoc["request_id"];
    }
    if (hasId && !hasReqId && !hasRequestId && !hasRequest_id) {
        if (!resp.contains("id")) {
            resp["id"] = reqDoc["id"];
        }
    }
    return resp.dump();
}

} // namespace

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string& message)>;
    using StreamEventCallback = std::function<void(const std::string& subId, uint64_t seq,
                                                   const std::string& line, const nlohmann::json& doc)>;

    WebSocketClient();
    ~WebSocketClient();

    bool Connect(const std::string& address, const std::string& token = "",
                 int timeoutMs = 15000, std::string* outError = nullptr);
    bool IsConnected() const;
    void Close();
    bool SendRequest(const std::string& requestJson, std::string& outResponse,
                     std::string& outError, int timeoutMs = 15000);
    void SetMessageCallback(MessageCallback cb);
    void SetStreamCallback(StreamEventCallback cb);
    std::string Endpoint() const;

    bool Subscribe(const std::string& stream, int id, uint64_t sinceSeq,
                   std::string& outSubId, std::string& outError, int timeoutMs = 15000);
    bool SubscribeFile(const std::string& filePath, uint64_t sinceSeq,
                       std::string& outSubId, std::string& outError, int timeoutMs = 15000);
    bool Unsubscribe(const std::string& subId, std::string& outError, int timeoutMs = 15000);

private:
    struct Impl {
        std::string address;
        std::string token;
        std::string endpoint;
        socket_t sock = kInvalidSocket;
        std::atomic<bool> connected{false};
        std::atomic<uint64_t> nextReqId{1};

        std::mutex sendMutex;
        std::mutex pendingMutex;
        std::unordered_map<std::string, std::shared_ptr<PendingRequest>> pendingRequests;

        std::mutex callbackMutex;
        MessageCallback onMessage;
        StreamEventCallback onStream;

        std::thread readThread;
    };
    std::shared_ptr<Impl> impl_;
};

WebSocketClient::WebSocketClient() : impl_(std::make_shared<Impl>()) {}

WebSocketClient::~WebSocketClient() {
    Close();
}

bool WebSocketClient::IsConnected() const {
    return impl_ && impl_->connected.load();
}

std::string WebSocketClient::Endpoint() const {
    return impl_ ? impl_->endpoint : std::string();
}

void WebSocketClient::SetMessageCallback(MessageCallback cb) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->callbackMutex);
    impl_->onMessage = std::move(cb);
}

void WebSocketClient::Close() {
    if (!impl_) return;
    if (impl_->connected.exchange(false)) {
        socket_t s = impl_->sock;
        impl_->sock = kInvalidSocket;
        if (s != kInvalidSocket) {
#ifdef _WIN32
            closesocket(s);
#else
            ::shutdown(s, SHUT_RDWR);
            ::close(s);
#endif
        }
    } else {
        if (impl_->sock != kInvalidSocket) {
            CloseSocket(impl_->sock);
            impl_->sock = kInvalidSocket;
        }
    }

    if (impl_->readThread.joinable()) {
        impl_->readThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        for (auto& kv : impl_->pendingRequests) {
            std::lock_guard<std::mutex> plock(kv.second->mtx);
            kv.second->done = true;
            kv.second->error = "client closed";
            kv.second->cv.notify_one();
        }
        impl_->pendingRequests.clear();
    }
}

bool WebSocketClient::Connect(const std::string& address, const std::string& token,
                             int timeoutMs, std::string* outError) {
    Close();

    std::string host;
    uint16_t port = 0;
    std::string err;
    if (!ParseRemoteAddress(address, host, port, err)) {
        if (outError) *outError = err;
        return false;
    }

    socket_t s = ConnectTcp(host, port, timeoutMs, err);
    if (s == kInvalidSocket) {
        if (outError) *outError = err;
        return false;
    }

    const std::string key = RandomWebSocketKey();
    std::string request;
    request += "GET /ipc HTTP/1.1\r\n";
    request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + key + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    if (!token.empty()) request += "Authorization: Bearer " + token + "\r\n";
    request += "\r\n";

    if (!SendAll(s, request.data(), request.size(), err)) {
        CloseSocket(s);
        if (outError) *outError = err;
        return false;
    }

    SockBuf handshakeReader;
    handshakeReader.s = s;
    handshakeReader.timeoutMs = timeoutMs;

    std::string headers;
    if (!handshakeReader.ReadUntil("\r\n\r\n", headers, err)) {
        CloseSocket(s);
        if (outError) *outError = err;
        return false;
    }
    if (headers.find(" 101 ") == std::string::npos && headers.rfind("HTTP/1.1 101", 0) != 0 &&
        headers.rfind("HTTP/1.0 101", 0) != 0) {
        CloseSocket(s);
        if (outError) *outError = "remote host refused the WebSocket upgrade: " + headers.substr(0, headers.find("\r\n"));
        return false;
    }
    const std::string expected = WebSocketAcceptFor(key);
    if (headers.find(expected) == std::string::npos) {
        CloseSocket(s);
        if (outError) *outError = "remote host returned an invalid WebSocket accept key";
        return false;
    }

    SetTimeouts(s, 0);

    impl_->sock = s;
    impl_->address = address;
    impl_->token = token;
    impl_->endpoint = "ws://" + host + ":" + std::to_string(port);
    impl_->connected.store(true);

    std::string leftover = std::move(handshakeReader.buffer);
    auto implCopy = impl_;
    impl_->readThread = std::thread([implCopy, leftover]() mutable {
        SockBuf reader;
        reader.s = implCopy->sock;
        reader.buffer = std::move(leftover);
        reader.timeoutMs = 0;

        while (implCopy->connected.load()) {
            std::string message;
            std::string readErr;
            if (!ReadWebSocketMessage(reader, true, message, readErr, &implCopy->sendMutex)) {
                break;
            }
            if (message.empty()) continue;

            nlohmann::json doc = nlohmann::json::parse(message, nullptr, false);
            std::string respReqId = ExtractReqId(doc);

            std::shared_ptr<PendingRequest> pending;
            if (!respReqId.empty()) {
                std::lock_guard<std::mutex> lock(implCopy->pendingMutex);
                auto it = implCopy->pendingRequests.find(respReqId);
                if (it != implCopy->pendingRequests.end()) {
                    pending = it->second;
                }
            }

            if (pending) {
                std::lock_guard<std::mutex> lock(pending->mtx);
                pending->done = true;
                pending->response = message;
                pending->cv.notify_one();
            } else {
                if (doc.is_object() && (doc.value("action", "") == "stream" || doc.value("event", "") == "stream" || doc.contains("sub_id"))) {
                    StreamEventCallback scb;
                    {
                        std::lock_guard<std::mutex> lock(implCopy->callbackMutex);
                        scb = implCopy->onStream;
                    }
                    if (scb) {
                        std::string subId = doc.value("sub_id", "");
                        uint64_t seq = doc.value("seq", uint64_t{0});
                        std::string line = doc.value("line", "");
                        scb(subId, seq, line, doc);
                    }
                }

                MessageCallback cb;
                {
                    std::lock_guard<std::mutex> lock(implCopy->callbackMutex);
                    cb = implCopy->onMessage;
                }
                if (cb) {
                    cb(message);
                }
            }
        }

        implCopy->connected.store(false);
        {
            std::lock_guard<std::mutex> lock(implCopy->pendingMutex);
            for (auto& kv : implCopy->pendingRequests) {
                std::lock_guard<std::mutex> plock(kv.second->mtx);
                kv.second->done = true;
                kv.second->error = "connection closed by server";
                kv.second->cv.notify_one();
            }
            implCopy->pendingRequests.clear();
        }
    });

    return true;
}

bool WebSocketClient::SendRequest(const std::string& requestJson, std::string& outResponse,
                                  std::string& outError, int timeoutMs) {
    outResponse.clear();
    outError.clear();

    if (!impl_ || !impl_->connected.load() || impl_->sock == kInvalidSocket) {
        outError = "WebSocket client is not connected";
        return false;
    }

    nlohmann::json reqDoc = nlohmann::json::parse(requestJson, nullptr, false);
    if (!reqDoc.is_object()) {
        outError = "invalid JSON request";
        return false;
    }

    if (!impl_->token.empty() && !reqDoc.contains("token")) {
        reqDoc["token"] = impl_->token;
    }

    std::string reqId;
    if (reqDoc.contains("req_id")) {
        reqId = reqDoc["req_id"].is_string() ? reqDoc["req_id"].get<std::string>() : reqDoc["req_id"].dump();
    } else if (reqDoc.contains("requestId")) {
        reqId = reqDoc["requestId"].is_string() ? reqDoc["requestId"].get<std::string>() : reqDoc["requestId"].dump();
    } else if (reqDoc.contains("request_id")) {
        reqId = reqDoc["request_id"].is_string() ? reqDoc["request_id"].get<std::string>() : reqDoc["request_id"].dump();
    } else if (reqDoc.contains("id") && reqDoc["id"].is_string()) {
        reqId = reqDoc["id"].get<std::string>();
    } else {
        reqId = "req_" + std::to_string(impl_->nextReqId.fetch_add(1));
        reqDoc["req_id"] = reqId;
    }

    auto pending = std::make_shared<PendingRequest>();
    {
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        impl_->pendingRequests[reqId] = pending;
    }

    const std::string payload = reqDoc.dump();
    const std::string frame = EncodeFrame(payload, 0x1, true); // masked
    std::string sendErr;
    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(impl_->sendMutex);
        if (impl_->sock != kInvalidSocket) {
            sent = SendAll(impl_->sock, frame.data(), frame.size(), sendErr);
        } else {
            sendErr = "socket was closed";
        }
    }

    if (!sent) {
        {
            std::lock_guard<std::mutex> lock(impl_->pendingMutex);
            impl_->pendingRequests.erase(reqId);
        }
        outError = "failed to send request frame: " + sendErr;
        return false;
    }

    if (timeoutMs <= 0) timeoutMs = 15000;
    std::unique_lock<std::mutex> lock(pending->mtx);
    bool ok = pending->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return pending->done;
    });

    {
        std::lock_guard<std::mutex> plock(impl_->pendingMutex);
        impl_->pendingRequests.erase(reqId);
    }

    if (!ok) {
        outError = "request timed out waiting for response (req_id=" + reqId + ")";
        return false;
    }

    if (!pending->error.empty()) {
        outError = pending->error;
        return false;
    }

    outResponse = pending->response;
    return true;
}

void WebSocketClient::SetStreamCallback(StreamEventCallback cb) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->callbackMutex);
    impl_->onStream = std::move(cb);
}

bool WebSocketClient::Subscribe(const std::string& stream, int id, uint64_t sinceSeq,
                                std::string& outSubId, std::string& outError, int timeoutMs) {
    nlohmann::json req;
    req["action"] = "subscribe";
    req["stream"] = stream;
    req["id"] = id;
    req["seq"] = sinceSeq;
    std::string resp;
    if (!SendRequest(req.dump(), resp, outError, timeoutMs)) {
        return false;
    }
    nlohmann::json doc = nlohmann::json::parse(resp, nullptr, false);
    if (!doc.is_object() || !doc.value("ok", false)) {
        outError = doc.is_object() ? doc.value("error", "subscribe failed") : "invalid response";
        return false;
    }
    outSubId = doc.value("sub_id", "");
    return true;
}

bool WebSocketClient::SubscribeFile(const std::string& filePath, uint64_t sinceSeq,
                                    std::string& outSubId, std::string& outError, int timeoutMs) {
    nlohmann::json req;
    req["action"] = "subscribe";
    req["file"] = filePath;
    req["seq"] = sinceSeq;
    std::string resp;
    if (!SendRequest(req.dump(), resp, outError, timeoutMs)) {
        return false;
    }
    nlohmann::json doc = nlohmann::json::parse(resp, nullptr, false);
    if (!doc.is_object() || !doc.value("ok", false)) {
        outError = doc.is_object() ? doc.value("error", "subscribe failed") : "invalid response";
        return false;
    }
    outSubId = doc.value("sub_id", "");
    return true;
}

bool WebSocketClient::Unsubscribe(const std::string& subId, std::string& outError, int timeoutMs) {
    nlohmann::json req;
    req["action"] = "unsubscribe";
    req["sub_id"] = subId;
    std::string resp;
    if (!SendRequest(req.dump(), resp, outError, timeoutMs)) {
        return false;
    }
    nlohmann::json doc = nlohmann::json::parse(resp, nullptr, false);
    if (!doc.is_object() || !doc.value("ok", false)) {
        outError = doc.is_object() ? doc.value("error", "unsubscribe failed") : "invalid response";
        return false;
    }
    return true;
}

namespace {
static std::mutex g_clientPoolMutex;
static std::unordered_map<std::string, std::shared_ptr<WebSocketClient>> g_clientPool;
} // namespace

bool WebSocketMultiplexRequest(const std::string& address, const std::string& token,
                               const std::string& requestJson, std::string& outResponse,
                               std::string& outError, int timeoutMs) {
    const std::string key = address + "\n" + token;
    std::shared_ptr<WebSocketClient> client;
    {
        std::lock_guard<std::mutex> lock(g_clientPoolMutex);
        auto it = g_clientPool.find(key);
        if (it != g_clientPool.end() && it->second) {
            client = it->second;
        } else {
            client = std::make_shared<WebSocketClient>();
            g_clientPool[key] = client;
        }
    }

    if (!client->IsConnected()) {
        if (!client->Connect(address, token, timeoutMs, &outError)) {
            return false;
        }
    }

    if (client->SendRequest(requestJson, outResponse, outError, timeoutMs)) {
        return true;
    }

    if (!client->IsConnected() || outError.find("closed") != std::string::npos) {
        client->Close();
        if (client->Connect(address, token, timeoutMs, &outError)) {
            return client->SendRequest(requestJson, outResponse, outError, timeoutMs);
        }
    }
    return false;
}

void ResetWebSocketPool() {
    std::lock_guard<std::mutex> lock(g_clientPoolMutex);
    for (auto& kv : g_clientPool) {
        if (kv.second) kv.second->Close();
    }
    g_clientPool.clear();
}

bool WebSocketRequest(const std::string& address, const std::string& token,
                      const std::string& requestJson, std::string& outResponse,
                      std::string& outError, int timeoutMs) {
    return WebSocketMultiplexRequest(address, token, requestJson, outResponse, outError, timeoutMs);
}

bool CallHost(const RemoteHost& host, const std::string& requestJson,
              std::string& outResponse, std::string& outError, int timeoutMs) {
    outResponse.clear();
    outError.clear();
    if (!host.enabled) {
        outError = "host '" + host.name + "' is disabled";
        return false;
    }
    const std::string token = ResolveHostToken(host);

    std::string request = requestJson;
    nlohmann::json req = nlohmann::json::parse(request, nullptr, false);
    if (req.is_object() && !token.empty()) {
        req["token"] = token;
        request = req.dump();
    }
    return WebSocketRequest(host.address, token, request, outResponse, outError, timeoutMs);
}

bool CallHost(const std::string& hostName, const std::string& requestJson,
              std::string& outResponse, std::string& outError, int timeoutMs) {
    HostRegistry registry;
    std::string loadError;
    if (!registry.Load(loadError)) {
        outError = loadError;
        return false;
    }
    RemoteHost host;
    if (!registry.Get(hostName, host)) {
        outError = "host not registered: " + hostName + " (add it to hosts.json)";
        return false;
    }
    return CallHost(host, requestJson, outResponse, outError, timeoutMs);
}

// ---------------------------------------------------------------------------
// Local remote listener
// ---------------------------------------------------------------------------

struct RemoteListener::State {
    std::atomic<bool> running{false};
    std::atomic<int> activeHandlers{0};
    std::string token;
    std::string bindHost;
    std::string endpoint;
    uint16_t port = 0;
    socket_t listenSocket = kInvalidSocket;
    mutable std::mutex handlerMutex;
    Handler handler;
    mutable std::mutex httpHandlerMutex;
    HttpHandler httpHandler;

    mutable std::mutex clientSocketsMutex;
    std::vector<socket_t> clientSockets;
    std::atomic<uint64_t> nextSubId{1};
};

namespace {

void HandleRemoteClient(std::shared_ptr<RemoteListener::State> state, socket_t client,
                        sockaddr_storage peer) {
    SetTimeouts(client, 15000);
    SockBuf reader;
    reader.s = client;
    reader.timeoutMs = 15000;

    std::string headers;
    std::string error;
    if (!reader.ReadUntil("\r\n\r\n", headers, error)) {
        return;
    }

    const bool loopback = IsLoopbackAddress(peer);
    const std::string peerText = PeerAddressText(peer);
    const bool webSocket = ContainsNoCase(headers, "upgrade: websocket");

    if (!webSocket) {
        RemoteListener::HttpHandler httpHandlerCopy;
        {
            std::lock_guard<std::mutex> lock(state->httpHandlerMutex);
            httpHandlerCopy = state->httpHandler;
        }

        bool looksLikeHttp = false;
        std::string method, uri;
        size_t firstLineEnd = headers.find("\r\n");
        if (firstLineEnd != std::string::npos) {
            std::string firstLine = headers.substr(0, firstLineEnd);
            size_t sp1 = firstLine.find(' ');
            if (sp1 != std::string::npos) {
                method = firstLine.substr(0, sp1);
                size_t sp2 = firstLine.find(' ', sp1 + 1);
                if (sp2 != std::string::npos) {
                    uri = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);
                    std::string proto = firstLine.substr(sp2 + 1);
                    if (proto.rfind("HTTP/", 0) == 0) {
                        looksLikeHttp = true;
                    }
                }
            }
        }

        if (looksLikeHttp && httpHandlerCopy) {
            std::string body;
            size_t clPos = LowerAscii(headers).find("content-length:");
            if (clPos != std::string::npos) {
                size_t valStart = clPos + std::strlen("content-length:");
                size_t valEnd = headers.find("\r\n", valStart);
                std::string clStr = headers.substr(valStart, valEnd == std::string::npos ? std::string::npos : valEnd - valStart);
                try {
                    long long cl = std::stoll(clStr);
                    if (cl > 0 && cl < 10 * 1024 * 1024) {
                        while (reader.buffer.size() < static_cast<size_t>(cl)) {
                            char buf[4096];
                            int n = ::recv(client, buf, sizeof(buf), 0);
                            if (n <= 0) break;
                            reader.buffer.append(buf, n);
                        }
                        if (reader.buffer.size() >= static_cast<size_t>(cl)) {
                            body = reader.buffer.substr(0, cl);
                            reader.buffer.erase(0, cl);
                        }
                    }
                } catch (...) {}
            }

            std::string httpResponse;
            if (httpHandlerCopy(method, uri, headers, body, peerText, httpResponse)) {
                SendAll(client, httpResponse.data(), httpResponse.size(), error);
                return;
            }
        }

        // Plain TCP fallback: one newline-terminated (or EOF-terminated) JSON request.
        std::string request;
        size_t newline = reader.buffer.find('\n');
        if (newline != std::string::npos) {
            request = reader.buffer.substr(0, newline);
            reader.buffer.erase(0, newline + 1);
        } else if (!reader.ReadToEofOrTimeout(request, error)) {
            return;
        }
        while (!request.empty() && (request.back() == '\r' || request.back() == '\n')) request.pop_back();

        const std::string token = RequestToken(request);
        std::string failure;
        if (!state->token.empty()) {
            if (token != state->token) failure = "invalid or missing token";
        } else if (!loopback) {
            failure = "authentication required for remote connections";
        }
        if (!failure.empty()) {
            const std::string body = JsonError(failure);
            SendAll(client, body.data(), body.size(), error);
            return;
        }

        std::string response;
        state->activeHandlers.fetch_add(1);
        try {
            RemoteListener::Handler handlerCopy;
            {
                std::lock_guard<std::mutex> lock(state->handlerMutex);
                handlerCopy = state->handler;
            }
            if (handlerCopy) response = handlerCopy(request, peerText);
        } catch (...) {
            response = JsonError("internal error while handling the request");
        }
        state->activeHandlers.fetch_sub(1);
        if (response.empty()) response = JsonError("no IPC handler is installed");
        SendAll(client, response.data(), response.size(), error);
        return;
    }

    // WebSocket multiplexed connection
    std::string key;
    size_t keyPos = LowerAscii(headers).find("sec-websocket-key:");
    if (keyPos != std::string::npos) {
        size_t valueStart = keyPos + std::strlen("sec-websocket-key:");
        size_t valueEnd = headers.find("\r\n", valueStart);
        key = headers.substr(valueStart, valueEnd == std::string::npos ? std::string::npos
                                                                       : valueEnd - valueStart);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
    }
    if (key.empty()) {
        return;
    }

    // Defense-in-depth: If httpHandler is set, verify Host & Origin
    {
        bool hasHttp = false;
        {
            std::lock_guard<std::mutex> lock(state->httpHandlerMutex);
            hasHttp = (state->httpHandler != nullptr);
        }
        if (hasHttp) {
            std::string hostVal;
            size_t hPos = LowerAscii(headers).find("host:");
            if (hPos != std::string::npos) {
                size_t lineEnd = headers.find("\r\n", hPos);
                hostVal = headers.substr(hPos + 5, lineEnd == std::string::npos ? std::string::npos : lineEnd - (hPos + 5));
                hostVal.erase(0, hostVal.find_first_not_of(" \t"));
                hostVal.erase(hostVal.find_last_not_of(" \t\r\n") + 1);
            }
            std::string h = hostVal;
            size_t colon = h.find(':');
            if (colon != std::string::npos) h = h.substr(0, colon);
            for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!h.empty() && h != "localhost" && h != "127.0.0.1" && h != "[::1]" && h != "::1") {
                std::string forbidden = "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: 23\r\n\r\nForbidden: invalid Host\n";
                SendAll(client, forbidden.data(), forbidden.size(), error);
                return;
            }

            std::string originVal;
            size_t oPos = LowerAscii(headers).find("origin:");
            if (oPos != std::string::npos) {
                size_t lineEnd = headers.find("\r\n", oPos);
                originVal = headers.substr(oPos + 7, lineEnd == std::string::npos ? std::string::npos : lineEnd - (oPos + 7));
                originVal.erase(0, originVal.find_first_not_of(" \t"));
                originVal.erase(originVal.find_last_not_of(" \t\r\n") + 1);
            }
            std::string o = originVal;
            for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!o.empty() && o != "null" &&
                o.rfind("http://localhost:", 0) != 0 && o != "http://localhost" &&
                o.rfind("http://127.0.0.1:", 0) != 0 && o != "http://127.0.0.1" &&
                o.rfind("http://[::1]:", 0) != 0 && o != "http://[::1]" &&
                o.rfind("https://localhost:", 0) != 0 && o != "https://localhost" &&
                o.rfind("https://127.0.0.1:", 0) != 0 && o != "https://127.0.0.1") {
                std::string forbidden = "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: 25\r\n\r\nForbidden: invalid Origin\n";
                SendAll(client, forbidden.data(), forbidden.size(), error);
                return;
            }
        }
    }

    std::string subprotocol;
    std::string protoToken;
    size_t protoPos = LowerAscii(headers).find("sec-websocket-protocol:");
    if (protoPos != std::string::npos) {
        size_t lineEnd = headers.find("\r\n", protoPos);
        std::string protoLine = headers.substr(protoPos + 23, lineEnd == std::string::npos ? std::string::npos : lineEnd - (protoPos + 23));
        size_t comma = protoLine.find(',');
        if (comma != std::string::npos) {
            std::string p1 = protoLine.substr(0, comma);
            std::string p2 = protoLine.substr(comma + 1);
            p1.erase(0, p1.find_first_not_of(" \t")); p1.erase(p1.find_last_not_of(" \t") + 1);
            p2.erase(0, p2.find_first_not_of(" \t")); p2.erase(p2.find_last_not_of(" \t") + 1);
            if (p1 == "hancho-auth") {
                subprotocol = "hancho-auth";
                protoToken = p2;
            }
        } else {
            std::string p = protoLine;
            p.erase(0, p.find_first_not_of(" \t")); p.erase(p.find_last_not_of(" \t") + 1);
            if (p == "hancho-auth") subprotocol = "hancho-auth";
        }
    }

    std::string upgradeResp = "HTTP/1.1 101 Switching Protocols\r\n";
    upgradeResp += "Upgrade: websocket\r\n";
    upgradeResp += "Connection: Upgrade\r\n";
    if (!subprotocol.empty()) {
        upgradeResp += "Sec-WebSocket-Protocol: " + subprotocol + "\r\n";
    }
    upgradeResp += "Sec-WebSocket-Accept: " + WebSocketAcceptFor(key) + "\r\n\r\n";
    if (!SendAll(client, upgradeResp.data(), upgradeResp.size(), error)) {
        return;
    }

    SetTimeouts(client, 0);
    reader.timeoutMs = 0;

    bool authenticated = false;
    if (state->token.empty()) {
        if (loopback) authenticated = true;
    } else {
        std::string headerToken;
        size_t authPos = LowerAscii(headers).find("authorization:");
        if (authPos != std::string::npos) {
            size_t lineEnd = headers.find("\r\n", authPos);
            std::string authLine = headers.substr(authPos, lineEnd == std::string::npos ? std::string::npos : lineEnd - authPos);
            size_t bearerPos = LowerAscii(authLine).find("bearer ");
            if (bearerPos != std::string::npos) {
                headerToken = authLine.substr(bearerPos + 7);
                headerToken.erase(0, headerToken.find_first_not_of(" \t\r\n"));
                headerToken.erase(headerToken.find_last_not_of(" \t\r\n") + 1);
            }
        }
        std::string uriToken;
        size_t sp1 = headers.find(' ');
        if (sp1 != std::string::npos) {
            size_t sp2 = headers.find(' ', sp1 + 1);
            if (sp2 != std::string::npos) {
                std::string reqUri = headers.substr(sp1 + 1, sp2 - sp1 - 1);
                size_t qPos = reqUri.find("token=");
                if (qPos != std::string::npos) {
                    size_t valEnd = reqUri.find('&', qPos);
                    uriToken = reqUri.substr(qPos + 6, valEnd == std::string::npos ? std::string::npos : valEnd - (qPos + 6));
                }
            }
        }
        if ((!headerToken.empty() && headerToken == state->token) ||
            (!protoToken.empty() && protoToken == state->token) ||
            (!uriToken.empty() && uriToken == state->token)) {
            authenticated = true;
        }
    }

    auto sendMutex = std::make_shared<std::mutex>();
    auto activeInFlight = std::make_shared<std::atomic<int>>(0);

    struct ActiveSub {
        std::string subId;
        std::shared_ptr<std::atomic<bool>> stopToken;
        std::thread watcher;
    };
    auto subsMutex = std::make_shared<std::mutex>();
    auto activeSubs = std::make_shared<std::unordered_map<std::string, std::shared_ptr<ActiveSub>>>();
    auto authShared = std::make_shared<std::atomic<bool>>(authenticated);

    while (state->running.load()) {
        std::string request;
        std::string readErr;
        if (!ReadWebSocketMessage(reader, false, request, readErr, sendMutex.get())) {
            break;
        }
        if (request.empty()) continue;

        activeInFlight->fetch_add(1);
        state->activeHandlers.fetch_add(1);

        std::thread([state, client, sendMutex, activeInFlight, request, peerText, loopback, authShared, subsMutex, activeSubs]() mutable {
            nlohmann::json reqDoc = nlohmann::json::parse(request, nullptr, false);

            std::string failure;
            if (!state->token.empty()) {
                if (!authShared->load()) {
                    std::string tok = RequestToken(request);
                    if (tok == state->token) {
                        authShared->store(true);
                    } else {
                        failure = "invalid or missing token";
                    }
                }
            } else if (!loopback && !authShared->load()) {
                failure = "authentication required for remote connections";
            }

            std::string action;
            if (reqDoc.is_object()) {
                action = reqDoc.value("action", "");
            }

            std::string response;
            if (action == "auth") {
                std::string tok = RequestToken(request);
                if (!state->token.empty() && tok == state->token) {
                    authShared->store(true);
                    response = "{\"ok\":true,\"action\":\"auth\"}";
                } else if (state->token.empty()) {
                    authShared->store(true);
                    response = "{\"ok\":true,\"action\":\"auth\"}";
                } else {
                    response = "{\"ok\":false,\"action\":\"auth\",\"error\":\"invalid token\"}";
                }
            } else if (action == "ping") {
                response = "{\"ok\":true,\"pong\":true}";
            } else if (!failure.empty()) {
                response = JsonError(failure);
            } else if (action == "unsubscribe") {
                std::string targetSubId = reqDoc.value("sub_id", "");
                bool found = false;
                {
                    std::lock_guard<std::mutex> lock(*subsMutex);
                    auto it = activeSubs->find(targetSubId);
                    if (it != activeSubs->end()) {
                        it->second->stopToken->store(true);
                        if (it->second->watcher.joinable()) {
                            it->second->watcher.detach();
                        }
                        activeSubs->erase(it);
                        found = true;
                    }
                }
                nlohmann::json unsubResp;
                unsubResp["ok"] = found;
                unsubResp["action"] = "unsubscribe";
                unsubResp["sub_id"] = targetSubId;
                if (!found) {
                    unsubResp["error"] = "subscription not found: " + targetSubId;
                }
                response = unsubResp.dump();
            } else if (action == "subscribe") {
                std::string subId = "sub_" + std::to_string(state->nextSubId.fetch_add(1));
                reqDoc["sub_id"] = subId;

                RemoteListener::Handler handlerCopy;
                {
                    std::lock_guard<std::mutex> lock(state->handlerMutex);
                    handlerCopy = state->handler;
                }
                std::string handlerResp;
                if (handlerCopy) {
                    try {
                        handlerResp = handlerCopy(reqDoc.dump(), peerText);
                    } catch (...) {
                        handlerResp = JsonError("internal error while handling subscribe");
                    }
                }

                nlohmann::json respDoc = nlohmann::json::parse(handlerResp, nullptr, false);
                if (!respDoc.is_object() || !respDoc.value("ok", false)) {
                    // Fallback for direct file or path
                    if (reqDoc.contains("file") || reqDoc.contains("path")) {
                        std::string fPath = reqDoc.contains("file") ? reqDoc["file"].get<std::string>()
                                                                    : reqDoc["path"].get<std::string>();
                        respDoc = nlohmann::json::object();
                        respDoc["ok"] = true;
                        respDoc["action"] = "subscribe";
                        respDoc["sub_id"] = subId;
                        respDoc["file"] = ResolveAbsolutePathUtf8(fPath);
                        respDoc["stream"] = reqDoc.value("stream", "file");
                        respDoc["seq"] = reqDoc.value("seq", uint64_t{0});
                        if (reqDoc.contains("id")) respDoc["id"] = reqDoc["id"];
                    }
                }

                if (respDoc.is_object() && respDoc.value("ok", false)) {
                    respDoc["sub_id"] = subId;
                    respDoc["action"] = "subscribe";

                    std::string filePath = respDoc.value("file", "");
                    std::string streamType = respDoc.value("stream", "events");
                    uint64_t startSeq = respDoc.value("seq", uint64_t{0});
                    int pId = respDoc.value("id", 0);

                    auto sub = std::make_shared<ActiveSub>();
                    sub->subId = subId;
                    sub->stopToken = std::make_shared<std::atomic<bool>>(false);
                    auto stopToken = sub->stopToken;

                    sub->watcher = std::thread([stopToken, client, sendMutex, subId, filePath, streamType, pId, startSeq]() mutable {
                        std::wstring pathW = Utf8ToWide(filePath);
                        uint64_t lastSeenSeq = startSeq;
                        uint64_t currentLineIndex = 0;
                        size_t lastByteOffset = 0;

                        while (!stopToken->load()) {
                            if (!PathExistsWide(pathW)) {
                                SleepMs(30);
                                continue;
                            }

                            std::ifstream in(pathW, std::ios::binary);
                            if (!in) {
                                SleepMs(30);
                                continue;
                            }

                            if (lastByteOffset > 0) {
                                in.seekg(lastByteOffset);
                            } else {
                                currentLineIndex = 0;
                            }

                            bool fileOk = true;
                            while (!stopToken->load() && fileOk) {
                                std::streampos posBefore = in.tellg();
                                std::string line;
                                if (!std::getline(in, line)) {
                                    in.clear();
                                    lastByteOffset = static_cast<size_t>(posBefore);
                                    SleepMs(30);

                                    std::error_code ec;
                                    auto curSize = std::filesystem::file_size(pathW, ec);
                                    if (!ec && curSize < lastByteOffset) {
                                        lastByteOffset = 0;
                                        currentLineIndex = 0;
                                        fileOk = false;
                                    }
                                    continue;
                                }

                                if (in.eof()) {
                                    in.clear();
                                    in.seekg(posBefore);
                                    SleepMs(30);
                                    continue;
                                }

                                lastByteOffset = static_cast<size_t>(in.tellg());
                                currentLineIndex++;

                                if (!line.empty() && line.back() == '\r') {
                                    line.pop_back();
                                }
                                if (line.empty()) {
                                    continue;
                                }

                                uint64_t evSeq = currentLineIndex;
                                nlohmann::json parsedDoc = nlohmann::json::parse(line, nullptr, false);
                                bool isJson = parsedDoc.is_object();
                                if (isJson && parsedDoc.contains("seq") && parsedDoc["seq"].is_number()) {
                                    evSeq = parsedDoc["seq"].get<uint64_t>();
                                }

                                if (evSeq <= lastSeenSeq) {
                                    continue;
                                }

                                nlohmann::json eventMsg;
                                eventMsg["action"] = "stream";
                                eventMsg["event"] = "stream";
                                eventMsg["sub_id"] = subId;
                                if (pId > 0) eventMsg["id"] = pId;
                                eventMsg["stream"] = streamType;
                                eventMsg["seq"] = evSeq;
                                if (isJson) eventMsg["data"] = parsedDoc;
                                eventMsg["line"] = line;

                                std::string frame = EncodeFrame(eventMsg.dump(), 0x1, false);
                                {
                                    std::lock_guard<std::mutex> lock(*sendMutex);
                                    std::string sendErr;
                                    if (!SendAll(client, frame.data(), frame.size(), sendErr)) {
                                        stopToken->store(true);
                                        break;
                                    }
                                }
                                lastSeenSeq = evSeq;
                            }
                        }
                    });

                    {
                        std::lock_guard<std::mutex> lock(*subsMutex);
                        (*activeSubs)[subId] = sub;
                    }
                    response = respDoc.dump();
                } else {
                    response = handlerResp.empty() ? JsonError("subscription target could not be resolved") : handlerResp;
                }
            } else {
                RemoteListener::Handler handlerCopy;
                {
                    std::lock_guard<std::mutex> lock(state->handlerMutex);
                    handlerCopy = state->handler;
                }
                if (handlerCopy) {
                    try {
                        response = handlerCopy(request, peerText);
                    } catch (...) {
                        response = JsonError("internal error while handling the request");
                    }
                } else {
                    response = JsonError("no IPC handler is installed");
                }
            }

            if (response.empty()) {
                response = JsonError("no IPC handler is installed");
            }

            response = AttachRequestId(response, reqDoc);

            {
                std::lock_guard<std::mutex> lock(*sendMutex);
                std::string sendErr;
                std::string frame = EncodeFrame(response, 0x1, false);
                SendAll(client, frame.data(), frame.size(), sendErr);
            }

            activeInFlight->fetch_sub(1);
            state->activeHandlers.fetch_sub(1);
        }).detach();
    }

    {
        std::lock_guard<std::mutex> lock(*subsMutex);
        for (auto& kv : *activeSubs) {
            kv.second->stopToken->store(true);
            if (kv.second->watcher.joinable()) {
                kv.second->watcher.detach();
            }
        }
        activeSubs->clear();
    }

    for (int i = 0; i < 300 && activeInFlight->load() > 0; ++i) {
        SleepMs(10);
    }
}

void RunRemoteAccept(std::shared_ptr<RemoteListener::State> state) {
    while (state->running.load()) {
        sockaddr_storage peer = {};
        socklen_t peerLen = sizeof(peer);
        socket_t client = ::accept(state->listenSocket, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (client == kInvalidSocket) {
            if (!state->running.load()) break;
            SleepMs(50);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(state->clientSocketsMutex);
            state->clientSockets.push_back(client);
        }
        std::thread([state, client, peer]() {
            HandleRemoteClient(state, client, peer);
            {
                std::lock_guard<std::mutex> lock(state->clientSocketsMutex);
                auto it = std::find(state->clientSockets.begin(), state->clientSockets.end(), client);
                if (it != state->clientSockets.end()) {
                    state->clientSockets.erase(it);
                }
            }
            CloseSocket(client);
        }).detach();
    }
}

} // namespace

RemoteListener::RemoteListener() : state_(std::make_shared<State>()) {}

RemoteListener::~RemoteListener() {
    Stop();
}

void RemoteListener::SetHandler(Handler handler) {
    std::lock_guard<std::mutex> lock(state_->handlerMutex);
    state_->handler = std::move(handler);
}

void RemoteListener::SetHttpHandler(HttpHandler httpHandler) {
    std::lock_guard<std::mutex> lock(state_->httpHandlerMutex);
    state_->httpHandler = std::move(httpHandler);
}

bool RemoteListener::Start(const std::string& bindHost, uint16_t port, const std::string& token,
                           std::string& outError) {
    outError.clear();
    if (state_->running.load()) {
        outError = "remote listener is already running";
        return false;
    }
    EnsureNetworking();

    const bool anyAddress = bindHost.empty() || bindHost == "*" || bindHost == "0.0.0.0";
    addrinfo hints = {};
    // "0.0.0.0" / "*" / empty mean the IPv4 wildcard in this code base. Prefer AF_INET
    // so a plain IPv4 LAN client can connect (getaddrinfo would otherwise return :: first).
    hints.ai_family = anyAddress ? AF_INET : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    int gai = getaddrinfo(anyAddress ? nullptr : bindHost.c_str(), portText.c_str(), &hints, &result);
    if (gai != 0 || !result) {
        outError = "cannot bind " + (anyAddress ? std::string("0.0.0.0") : bindHost) + ":" +
                   portText + ": " + std::to_string(gai);
        return false;
    }

    socket_t listener = kInvalidSocket;
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        socket_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kInvalidSocket) continue;
        int yes = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        if (::bind(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 && ::listen(s, 16) == 0) {
            listener = s;
            break;
        }
        CloseSocket(s);
    }
    freeaddrinfo(result);
    if (listener == kInvalidSocket) {
        outError = "cannot listen on " + (anyAddress ? std::string("0.0.0.0") : bindHost) + ":" +
                   portText;
        return false;
    }

    state_->token = token;
    state_->bindHost = anyAddress ? "0.0.0.0" : bindHost;
    uint16_t boundPort = port;
    if (boundPort == 0) {
        sockaddr_storage bound = {};
        socklen_t boundLen = sizeof(bound);
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
            if (bound.ss_family == AF_INET) {
                boundPort = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
            } else if (bound.ss_family == AF_INET6) {
                boundPort = ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
            }
        }
    }
    state_->port = boundPort;
    state_->listenSocket = listener;
    state_->endpoint = "ws://" + state_->bindHost + ":" + std::to_string(boundPort);
    state_->running.store(true);
    acceptThread_ = std::thread(RunRemoteAccept, state_);
    return true;
}

void RemoteListener::Stop() {
    if (!state_) return;
    if (!state_->running.exchange(false)) {
        return;
    }
    socket_t s = state_->listenSocket;
    state_->listenSocket = kInvalidSocket;
    if (s != kInvalidSocket) {
        // Shutting down the listening socket wakes accept() on both platforms.
#ifdef _WIN32
        closesocket(s);
#else
        ::shutdown(s, SHUT_RDWR);
        ::close(s);
#endif
    }

    {
        std::lock_guard<std::mutex> lock(state_->clientSocketsMutex);
        for (socket_t cs : state_->clientSockets) {
            if (cs != kInvalidSocket) {
#ifdef _WIN32
                closesocket(cs);
#else
                ::shutdown(cs, SHUT_RDWR);
                ::close(cs);
#endif
            }
        }
        state_->clientSockets.clear();
    }

    if (acceptThread_.joinable()) acceptThread_.join();
    // Give an in-flight IPC handler time to return before the owner is destroyed.
    // The cap keeps a stuck handler from blocking shutdown forever.
    for (int i = 0; i < 3000 && state_->activeHandlers.load() > 0; ++i) {
        SleepMs(10);
    }
}

bool RemoteListener::Running() const {
    return state_ && state_->running.load();
}

std::string RemoteListener::Endpoint() const {
    return state_ ? state_->endpoint : std::string();
}

} // namespace remote
} // namespace pc
