#pragma once

// WCSP v1.0 wire protocol.
//
// All fixed-width integers are little-endian.  Strings are UTF-8 and are
// carried inside TLVs with an explicit length (never NUL terminated).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wcs {

constexpr uint16_t kProtocolMajor = 1;
constexpr uint16_t kProtocolMinor = 0;

constexpr uint32_t kMaxPayloadBytes = 1024u * 1024u;
constexpr uint16_t kMaxDimension = 1000;
constexpr uint16_t kMinColumns = 2;
constexpr uint16_t kMinRows = 1;

// "WCSP"
constexpr uint8_t kMagic[4] = { 'W', 'C', 'S', 'P' };

enum class FrameType : uint8_t {
    Hello        = 0x01,
    HelloAck     = 0x02,
    Auth         = 0x05,
    AuthResult   = 0x06,
    Input        = 0x10,
    Resize       = 0x11,
    Resized      = 0x12,
    Close        = 0x13,
    Ping         = 0x14,
    Pong         = 0x15,
    WindowUpdate = 0x16,
    Output       = 0x20,
    Exit         = 0x22,
    Error        = 0x23,
    Title        = 0x24,
    Noop         = 0x7F,
};

enum FrameFlags : uint8_t {
    FlagNone     = 0x00,
    FlagPriority = 0x01,
};

enum Feature : uint32_t {
    FeatureResize           = 1u << 0,
    FeaturePriorityInput    = 1u << 1,
    FeatureFlowCredit       = 1u << 2,
    FeatureCompression      = 1u << 3,
    FeatureTls              = 1u << 4,
    FeatureImagePassthrough = 1u << 5,
    FeatureClipboardOsc52   = 1u << 6,
    FeatureExitCode         = 1u << 7,
};

constexpr uint32_t kDefaultFeatures =
    FeatureResize | FeaturePriorityInput | FeatureImagePassthrough |
    FeatureClipboardOsc52 | FeatureExitCode;

enum class ErrorCode : uint16_t {
    InvalidFrame     = 0x0001,
    FrameTooLarge    = 0x0002,
    InvalidHello     = 0x0010,
    VersionMismatch  = 0x0101,
    AuthRequired     = 0x0102,
    AuthFailed       = 0x0103,
    ShellNotAllowed  = 0x0104,
    SessionLimit     = 0x0105,
    ShellStartFailed = 0x0201,
    ConPtyUnavailable= 0x0202,
    Internal         = 0x0301,
};

enum class ExitReason : uint16_t {
    ChildExited   = 0,
    ServerShutdown= 1,
    ClientClose   = 2,
    Killed        = 3,
};

enum class ResizeReason : uint16_t {
    Applied = 0,
    Clamped = 1,
};

// TLV type ids used by HELLO / HELLO_ACK.
enum TlvType : uint16_t {
    TlvMagic      = 0x0000,
    TlvVersion    = 0x0001,
    TlvName       = 0x0002,
    TlvFeatures   = 0x0003,
    TlvSize       = 0x0004,
    TlvShell      = 0x0005,
    TlvToken      = 0x0006,
    TlvEnv        = 0x0007,
    TlvClientId   = 0x0008,
    TlvSessionId  = 0x0009,
    TlvEncoding   = 0x000A,
};

struct Frame {
    FrameType type = FrameType::Noop;
    uint8_t flags = FlagNone;
    uint16_t stream_id = 0;
    std::vector<uint8_t> payload;
};

struct Hello {
    uint16_t major = kProtocolMajor;
    uint16_t minor = kProtocolMinor;
    uint32_t features = kDefaultFeatures;
    uint16_t cols = 80;
    uint16_t rows = 24;
    std::string name = "WinConsoleTerminal";
    std::string shell;
    std::string token;
    std::string client_id;
    std::vector<std::string> env;
};

struct HelloAck {
    uint16_t major = kProtocolMajor;
    uint16_t minor = kProtocolMinor;
    uint32_t features = kDefaultFeatures;
    uint16_t cols = 80;
    uint16_t rows = 24;
    uint32_t session_id = 0;
    std::string name = "WinConsoleServer";
    std::string shell;
};

struct ResizeInfo {
    uint16_t cols = 80;
    uint16_t rows = 24;
};

struct ResizedInfo {
    uint16_t cols = 80;
    uint16_t rows = 24;
    ResizeReason reason = ResizeReason::Applied;
};

struct ExitInfo {
    ExitReason reason = ExitReason::ChildExited;
    int32_t exit_code = 0;
};

struct ErrorInfo {
    ErrorCode code = ErrorCode::Internal;
    std::string message;
};

// --- little-endian primitives -------------------------------------------------

void AppendU8(std::vector<uint8_t>& out, uint8_t v);
void AppendU16(std::vector<uint8_t>& out, uint16_t v);
void AppendU32(std::vector<uint8_t>& out, uint32_t v);
void AppendU64(std::vector<uint8_t>& out, uint64_t v);
void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t len);

bool ReadU8(const uint8_t* data, size_t len, size_t& off, uint8_t& v);
bool ReadU16(const uint8_t* data, size_t len, size_t& off, uint16_t& v);
bool ReadU32(const uint8_t* data, size_t len, size_t& off, uint32_t& v);
bool ReadU64(const uint8_t* data, size_t len, size_t& off, uint64_t& v);

// --- frame codec --------------------------------------------------------------

std::vector<uint8_t> EncodeFrame(const Frame& frame);
bool DecodeHeader(const uint8_t* data, size_t len, uint32_t& payload_len,
                  FrameType& type, uint8_t& flags, uint16_t& stream_id);

// --- TLV helpers --------------------------------------------------------------

void AppendTlv(std::vector<uint8_t>& out, uint16_t type,
               const void* data, size_t len);
void AppendTlvString(std::vector<uint8_t>& out, uint16_t type,
                     const std::string& value);
void AppendTlvU16(std::vector<uint8_t>& out, uint16_t type, uint16_t value);
void AppendTlvU32(std::vector<uint8_t>& out, uint16_t type, uint32_t value);

struct Tlv {
    uint16_t type = 0;
    std::vector<uint8_t> value;
};

bool DecodeTlvs(const uint8_t* data, size_t len, std::vector<Tlv>& out,
                std::string* error);
const Tlv* FindTlv(const std::vector<Tlv>& tlvs, uint16_t type);

// --- message codecs -----------------------------------------------------------

std::vector<uint8_t> EncodeHello(const Hello& hello);
bool DecodeHello(const uint8_t* data, size_t len, Hello& out, std::string* error);

std::vector<uint8_t> EncodeHelloAck(const HelloAck& ack);
bool DecodeHelloAck(const uint8_t* data, size_t len, HelloAck& out,
                    std::string* error);

std::vector<uint8_t> EncodeResize(const ResizeInfo& info);
bool DecodeResize(const uint8_t* data, size_t len, ResizeInfo& out);

std::vector<uint8_t> EncodeResized(const ResizedInfo& info);
bool DecodeResized(const uint8_t* data, size_t len, ResizedInfo& out);

std::vector<uint8_t> EncodeExit(const ExitInfo& info);
bool DecodeExit(const uint8_t* data, size_t len, ExitInfo& out);

std::vector<uint8_t> EncodeError(const ErrorInfo& info);
bool DecodeError(const uint8_t* data, size_t len, ErrorInfo& out);

std::vector<uint8_t> EncodePing(uint64_t nonce);
bool DecodePing(const uint8_t* data, size_t len, uint64_t& nonce);

} // namespace wcs
