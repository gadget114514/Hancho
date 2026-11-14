#pragma once

#include "protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wcs {

enum class ReadResult {
    Frame,
    NeedMore,
    Error,
};

// Incremental frame parser.  Feed it any number of bytes; it returns one frame
// at a time and keeps partial data for the next call.
class FrameReader {
public:
    explicit FrameReader(uint32_t max_payload = kMaxPayloadBytes)
        : max_payload_(max_payload) {}

    void Append(const uint8_t* data, size_t len);
    ReadResult Next(Frame& out, std::string* error);
    void Clear();

    size_t BufferedBytes() const { return buffer_.size() - offset_; }
    void SetMaxPayload(uint32_t bytes) { max_payload_ = bytes; }

private:
    std::vector<uint8_t> buffer_;
    size_t offset_ = 0;
    uint32_t max_payload_;
};

} // namespace wcs
