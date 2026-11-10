#include "remote_client.h"

#include "common.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
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
bool ReadWebSocketMessage(SockBuf& reader, bool clientRole, std::string& out, std::string& outError) {
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
            if (!SendAll(reader.s, pong.data(), pong.size(), outError)) return false;
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

} // namespace

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

bool WebSocketRequest(const std::string& address, const std::string& token,
                      const std::string& requestJson, std::string& outResponse,
                      std::string& outError, int timeoutMs) {
    outResponse.clear();
    outError.clear();

    std::string host;
    uint16_t port = 0;
    if (!ParseRemoteAddress(address, host, port, outError)) return false;

    socket_t s = ConnectTcp(host, port, timeoutMs, outError);
    if (s == kInvalidSocket) return false;

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

    if (!SendAll(s, request.data(), request.size(), outError)) {
        CloseSocket(s);
        return false;
    }

    SockBuf reader;
    reader.s = s;
    reader.timeoutMs = timeoutMs;

    std::string headers;
    if (!reader.ReadUntil("\r\n\r\n", headers, outError)) {
        CloseSocket(s);
        return false;
    }
    if (headers.find(" 101 ") == std::string::npos && headers.rfind("HTTP/1.1 101", 0) != 0 &&
        headers.rfind("HTTP/1.0 101", 0) != 0) {
        outError = "remote host refused the WebSocket upgrade: " + headers.substr(0, headers.find("\r\n"));
        CloseSocket(s);
        return false;
    }
    const std::string expected = WebSocketAcceptFor(key);
    if (headers.find(expected) == std::string::npos) {
        outError = "remote host returned an invalid WebSocket accept key";
        CloseSocket(s);
        return false;
    }

    const std::string frame = EncodeFrame(requestJson, 0x1, true);
    if (!SendAll(s, frame.data(), frame.size(), outError)) {
        CloseSocket(s);
        return false;
    }

    std::string message;
    if (!ReadWebSocketMessage(reader, true, message, outError)) {
        CloseSocket(s);
        return false;
    }
    CloseSocket(s);

    if (message.empty()) {
        outError = "remote host returned an empty response";
        return false;
    }
    outResponse = message;
    return true;
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
        CloseSocket(client);
        return;
    }

    const bool loopback = IsLoopbackAddress(peer);
    const std::string peerText = PeerAddressText(peer);
    const bool webSocket = ContainsNoCase(headers, "upgrade: websocket");

    std::string request;
    if (webSocket) {
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
            CloseSocket(client);
            return;
        }

        std::string response = "HTTP/1.1 101 Switching Protocols\r\n";
        response += "Upgrade: websocket\r\n";
        response += "Connection: Upgrade\r\n";
        response += "Sec-WebSocket-Accept: " + WebSocketAcceptFor(key) + "\r\n\r\n";
        if (!SendAll(client, response.data(), response.size(), error)) {
            CloseSocket(client);
            return;
        }
        if (!ReadWebSocketMessage(reader, false, request, error)) {
            CloseSocket(client);
            return;
        }
    } else {
        // Plain TCP fallback: one newline-terminated (or EOF-terminated) JSON request.
        size_t newline = reader.buffer.find('\n');
        if (newline != std::string::npos) {
            request = reader.buffer.substr(0, newline);
            reader.buffer.erase(0, newline + 1);
        } else if (!reader.ReadToEofOrTimeout(request, error)) {
            CloseSocket(client);
            return;
        }
        while (!request.empty() && (request.back() == '\r' || request.back() == '\n')) request.pop_back();
    }

    const std::string token = RequestToken(request);
    std::string failure;
    if (!state->token.empty()) {
        // Once a token is configured it is required from every peer, including
        // loopback: an SSH tunnel also arrives as a loopback connection.
        if (token != state->token) failure = "invalid or missing token";
    } else if (!loopback) {
        failure = "authentication required for remote connections";
    }
    if (!failure.empty()) {
        const std::string body = JsonError(failure);
        if (webSocket) {
            const std::string frame = EncodeFrame(body, 0x1, false);
            SendAll(client, frame.data(), frame.size(), error);
        } else {
            SendAll(client, body.data(), body.size(), error);
        }
        CloseSocket(client);
        return;
    }

    std::string response;
    state->activeHandlers.fetch_add(1);
    try {
        std::lock_guard<std::mutex> lock(state->handlerMutex);
        if (state->handler) response = state->handler(request, peerText);
    } catch (...) {
        response = JsonError("internal error while handling the request");
    }
    state->activeHandlers.fetch_sub(1);
    if (response.empty()) response = JsonError("no IPC handler is installed");

    if (webSocket) {
        const std::string frame = EncodeFrame(response, 0x1, false);
        SendAll(client, frame.data(), frame.size(), error);
    } else {
        SendAll(client, response.data(), response.size(), error);
    }
    CloseSocket(client);
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
        std::thread(HandleRemoteClient, state, client, peer).detach();
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
    hints.ai_family = AF_UNSPEC;
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
