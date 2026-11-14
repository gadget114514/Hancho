#include "protocol.h"

#include <cstring>
#include <limits>

namespace wcs {
namespace {

bool Fail(std::string* error, const char* msg) {
    if (error) *error = msg;
    return false;
}

void AppendStringBytes(std::vector<uint8_t>& out, const std::string& s) {
    AppendBytes(out, s.data(), s.size());
}

std::string TlvString(const Tlv* tlv) {
    if (!tlv) return {};
    return std::string(reinterpret_cast<const char*>(tlv->value.data()),
                       tlv->value.size());
}

bool TlvU16(const Tlv* tlv, uint16_t& value) {
    if (!tlv || tlv->value.size() != 2) return false;
    value = static_cast<uint16_t>(tlv->value[0]) |
            static_cast<uint16_t>(tlv->value[1] << 8);
    return true;
}

bool TlvU32(const Tlv* tlv, uint32_t& value) {
    if (!tlv || tlv->value.size() != 4) return false;
    value = static_cast<uint32_t>(tlv->value[0]) |
            (static_cast<uint32_t>(tlv->value[1]) << 8) |
            (static_cast<uint32_t>(tlv->value[2]) << 16) |
            (static_cast<uint32_t>(tlv->value[3]) << 24);
    return true;
}

bool ParseTlvVersion(const Tlv* tlv, uint16_t& major, uint16_t& minor) {
    if (!tlv || tlv->value.size() != 4) return false;
    major = static_cast<uint16_t>(tlv->value[0]) |
            static_cast<uint16_t>(tlv->value[1] << 8);
    minor = static_cast<uint16_t>(tlv->value[2]) |
            static_cast<uint16_t>(tlv->value[3] << 8);
    return true;
}

bool ParseTlvSize(const Tlv* tlv, uint16_t& cols, uint16_t& rows) {
    if (!tlv || tlv->value.size() != 4) return false;
    cols = static_cast<uint16_t>(tlv->value[0]) |
           static_cast<uint16_t>(tlv->value[1] << 8);
    rows = static_cast<uint16_t>(tlv->value[2]) |
           static_cast<uint16_t>(tlv->value[3] << 8);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// primitives
// ---------------------------------------------------------------------------

void AppendU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void AppendU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void AppendU64(std::vector<uint8_t>& out, uint64_t v) {
    AppendU32(out, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    AppendU32(out, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t len) {
    if (!data || len == 0) return;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + len);
}

bool ReadU8(const uint8_t* data, size_t len, size_t& off, uint8_t& v) {
    if (off + 1 > len) return false;
    v = data[off++];
    return true;
}

bool ReadU16(const uint8_t* data, size_t len, size_t& off, uint16_t& v) {
    if (off + 2 > len) return false;
    v = static_cast<uint16_t>(data[off]) |
        static_cast<uint16_t>(data[off + 1] << 8);
    off += 2;
    return true;
}

bool ReadU32(const uint8_t* data, size_t len, size_t& off, uint32_t& v) {
    if (off + 4 > len) return false;
    v = static_cast<uint32_t>(data[off]) |
        (static_cast<uint32_t>(data[off + 1]) << 8) |
        (static_cast<uint32_t>(data[off + 2]) << 16) |
        (static_cast<uint32_t>(data[off + 3]) << 24);
    off += 4;
    return true;
}

bool ReadU64(const uint8_t* data, size_t len, size_t& off, uint64_t& v) {
    uint32_t lo = 0, hi = 0;
    if (!ReadU32(data, len, off, lo) || !ReadU32(data, len, off, hi)) return false;
    v = (static_cast<uint64_t>(hi) << 32) | lo;
    return true;
}

// ---------------------------------------------------------------------------
// frame codec
// ---------------------------------------------------------------------------

std::vector<uint8_t> EncodeFrame(const Frame& frame) {
    std::vector<uint8_t> out;
    out.reserve(8 + frame.payload.size());
    AppendU32(out, static_cast<uint32_t>(frame.payload.size()));
    AppendU8(out, static_cast<uint8_t>(frame.type));
    AppendU8(out, frame.flags);
    AppendU16(out, frame.stream_id);
    AppendBytes(out, frame.payload.data(), frame.payload.size());
    return out;
}

bool DecodeHeader(const uint8_t* data, size_t len, uint32_t& payload_len,
                  FrameType& type, uint8_t& flags, uint16_t& stream_id) {
    if (len < 8) return false;
    size_t off = 0;
    uint8_t type8 = 0;
    if (!ReadU32(data, len, off, payload_len) ||
        !ReadU8(data, len, off, type8) ||
        !ReadU8(data, len, off, flags) ||
        !ReadU16(data, len, off, stream_id)) {
        return false;
    }
    type = static_cast<FrameType>(type8);
    return true;
}

// ---------------------------------------------------------------------------
// TLV
// ---------------------------------------------------------------------------

void AppendTlv(std::vector<uint8_t>& out, uint16_t type,
               const void* data, size_t len) {
    AppendU16(out, type);
    AppendU32(out, static_cast<uint32_t>(len));
    AppendBytes(out, data, len);
}

void AppendTlvString(std::vector<uint8_t>& out, uint16_t type,
                     const std::string& value) {
    AppendTlv(out, type, value.data(), value.size());
}

void AppendTlvU16(std::vector<uint8_t>& out, uint16_t type, uint16_t value) {
    std::vector<uint8_t> tmp;
    AppendU16(tmp, value);
    AppendTlv(out, type, tmp.data(), tmp.size());
}

void AppendTlvU32(std::vector<uint8_t>& out, uint16_t type, uint32_t value) {
    std::vector<uint8_t> tmp;
    AppendU32(tmp, value);
    AppendTlv(out, type, tmp.data(), tmp.size());
}

bool DecodeTlvs(const uint8_t* data, size_t len, std::vector<Tlv>& out,
                std::string* error) {
    out.clear();
    size_t off = 0;
    while (off < len) {
        uint16_t type = 0;
        uint32_t length = 0;
        if (!ReadU16(data, len, off, type) ||
            !ReadU32(data, len, off, length)) {
            return Fail(error, "truncated TLV header");
        }
        if (length > len - off) {
            return Fail(error, "truncated TLV value");
        }
        Tlv tlv;
        tlv.type = type;
        tlv.value.assign(data + off, data + off + length);
        off += length;
        out.push_back(std::move(tlv));
    }
    return true;
}

const Tlv* FindTlv(const std::vector<Tlv>& tlvs, uint16_t type) {
    for (const Tlv& tlv : tlvs) {
        if (tlv.type == type) return &tlv;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// HELLO / HELLO_ACK
// ---------------------------------------------------------------------------

std::vector<uint8_t> EncodeHello(const Hello& hello) {
    std::vector<uint8_t> out;

    AppendTlv(out, TlvMagic, kMagic, sizeof(kMagic));

    std::vector<uint8_t> version;
    AppendU16(version, hello.major);
    AppendU16(version, hello.minor);
    AppendTlv(out, TlvVersion, version.data(), version.size());

    AppendTlvString(out, TlvName, hello.name);
    AppendTlvU32(out, TlvFeatures, hello.features);

    std::vector<uint8_t> size;
    AppendU16(size, hello.cols);
    AppendU16(size, hello.rows);
    AppendTlv(out, TlvSize, size.data(), size.size());

    if (!hello.shell.empty()) AppendTlvString(out, TlvShell, hello.shell);
    if (!hello.token.empty()) AppendTlvString(out, TlvToken, hello.token);
    if (!hello.client_id.empty()) AppendTlvString(out, TlvClientId, hello.client_id);
    for (const std::string& env : hello.env) {
        AppendTlvString(out, TlvEnv, env);
    }
    return out;
}

bool DecodeHello(const uint8_t* data, size_t len, Hello& out,
                 std::string* error) {
    std::vector<Tlv> tlvs;
    if (!DecodeTlvs(data, len, tlvs, error)) return false;

    const Tlv* magic = FindTlv(tlvs, TlvMagic);
    if (!magic || magic->value.size() != 4 ||
        std::memcmp(magic->value.data(), kMagic, 4) != 0) {
        return Fail(error, "bad magic");
    }

    if (!ParseTlvVersion(FindTlv(tlvs, TlvVersion), out.major, out.minor)) {
        return Fail(error, "missing version");
    }
    if (!ParseTlvSize(FindTlv(tlvs, TlvSize), out.cols, out.rows)) {
        return Fail(error, "missing size");
    }

    const Tlv* features = FindTlv(tlvs, TlvFeatures);
    if (features) {
        uint32_t value = 0;
        if (!TlvU32(features, value)) return Fail(error, "bad features");
        out.features = value;
    } else {
        out.features = 0;
    }

    out.name = TlvString(FindTlv(tlvs, TlvName));
    out.shell = TlvString(FindTlv(tlvs, TlvShell));
    out.token = TlvString(FindTlv(tlvs, TlvToken));
    out.client_id = TlvString(FindTlv(tlvs, TlvClientId));

    out.env.clear();
    for (const Tlv& tlv : tlvs) {
        if (tlv.type == TlvEnv) out.env.push_back(TlvString(&tlv));
    }
    return true;
}

std::vector<uint8_t> EncodeHelloAck(const HelloAck& ack) {
    std::vector<uint8_t> out;

    std::vector<uint8_t> version;
    AppendU16(version, ack.major);
    AppendU16(version, ack.minor);
    AppendTlv(out, TlvVersion, version.data(), version.size());

    AppendTlvString(out, TlvName, ack.name);
    AppendTlvU32(out, TlvFeatures, ack.features);

    std::vector<uint8_t> size;
    AppendU16(size, ack.cols);
    AppendU16(size, ack.rows);
    AppendTlv(out, TlvSize, size.data(), size.size());

    AppendTlvU32(out, TlvSessionId, ack.session_id);
    if (!ack.shell.empty()) AppendTlvString(out, TlvShell, ack.shell);
    AppendTlvString(out, TlvEncoding, "UTF-8");
    return out;
}

bool DecodeHelloAck(const uint8_t* data, size_t len, HelloAck& out,
                    std::string* error) {
    std::vector<Tlv> tlvs;
    if (!DecodeTlvs(data, len, tlvs, error)) return false;

    if (!ParseTlvVersion(FindTlv(tlvs, TlvVersion), out.major, out.minor)) {
        return Fail(error, "missing version");
    }
    if (!ParseTlvSize(FindTlv(tlvs, TlvSize), out.cols, out.rows)) {
        return Fail(error, "missing size");
    }

    const Tlv* features = FindTlv(tlvs, TlvFeatures);
    if (features) {
        uint32_t value = 0;
        if (!TlvU32(features, value)) return Fail(error, "bad features");
        out.features = value;
    }
    const Tlv* session = FindTlv(tlvs, TlvSessionId);
    if (session) {
        uint32_t value = 0;
        if (!TlvU32(session, value)) return Fail(error, "bad session id");
        out.session_id = value;
    }
    out.name = TlvString(FindTlv(tlvs, TlvName));
    out.shell = TlvString(FindTlv(tlvs, TlvShell));
    return true;
}

// ---------------------------------------------------------------------------
// small messages
// ---------------------------------------------------------------------------

std::vector<uint8_t> EncodeResize(const ResizeInfo& info) {
    std::vector<uint8_t> out;
    AppendU16(out, info.cols);
    AppendU16(out, info.rows);
    return out;
}

bool DecodeResize(const uint8_t* data, size_t len, ResizeInfo& out) {
    size_t off = 0;
    return ReadU16(data, len, off, out.cols) &&
           ReadU16(data, len, off, out.rows);
}

std::vector<uint8_t> EncodeResized(const ResizedInfo& info) {
    std::vector<uint8_t> out;
    AppendU16(out, info.cols);
    AppendU16(out, info.rows);
    AppendU16(out, static_cast<uint16_t>(info.reason));
    return out;
}

bool DecodeResized(const uint8_t* data, size_t len, ResizedInfo& out) {
    size_t off = 0;
    uint16_t reason = 0;
    if (!ReadU16(data, len, off, out.cols) ||
        !ReadU16(data, len, off, out.rows) ||
        !ReadU16(data, len, off, reason)) {
        return false;
    }
    out.reason = static_cast<ResizeReason>(reason);
    return true;
}

std::vector<uint8_t> EncodeExit(const ExitInfo& info) {
    std::vector<uint8_t> out;
    AppendU16(out, static_cast<uint16_t>(info.reason));
    AppendU16(out, 0);
    AppendU32(out, static_cast<uint32_t>(info.exit_code));
    return out;
}

bool DecodeExit(const uint8_t* data, size_t len, ExitInfo& out) {
    size_t off = 0;
    uint16_t reason = 0;
    uint16_t reserved = 0;
    uint32_t code = 0;
    if (!ReadU16(data, len, off, reason) ||
        !ReadU16(data, len, off, reserved) ||
        !ReadU32(data, len, off, code)) {
        return false;
    }
    out.reason = static_cast<ExitReason>(reason);
    out.exit_code = static_cast<int32_t>(code);
    return true;
}

std::vector<uint8_t> EncodeError(const ErrorInfo& info) {
    std::vector<uint8_t> out;
    AppendU16(out, static_cast<uint16_t>(info.code));
    AppendU16(out, 0);
    AppendStringBytes(out, info.message);
    return out;
}

bool DecodeError(const uint8_t* data, size_t len, ErrorInfo& out) {
    size_t off = 0;
    uint16_t code = 0;
    uint16_t reserved = 0;
    if (!ReadU16(data, len, off, code) ||
        !ReadU16(data, len, off, reserved)) {
        return false;
    }
    out.code = static_cast<ErrorCode>(code);
    out.message.assign(reinterpret_cast<const char*>(data + off), len - off);
    return true;
}

std::vector<uint8_t> EncodePing(uint64_t nonce) {
    std::vector<uint8_t> out;
    AppendU64(out, nonce);
    return out;
}

bool DecodePing(const uint8_t* data, size_t len, uint64_t& nonce) {
    size_t off = 0;
    return ReadU64(data, len, off, nonce);
}

} // namespace wcs
