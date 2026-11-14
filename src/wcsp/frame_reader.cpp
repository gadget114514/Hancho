#include "frame_reader.h"

namespace wcs {

void FrameReader::Append(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    if (offset_ > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + offset_);
        offset_ = 0;
    }
    buffer_.insert(buffer_.end(), data, data + len);
}

void FrameReader::Clear() {
    buffer_.clear();
    offset_ = 0;
}

ReadResult FrameReader::Next(Frame& out, std::string* error) {
    const size_t available = buffer_.size() - offset_;
    if (available < 8) return ReadResult::NeedMore;

    const uint8_t* p = buffer_.data() + offset_;
    uint32_t payload_len = 0;
    FrameType type = FrameType::Noop;
    uint8_t flags = 0;
    uint16_t stream_id = 0;

    if (!DecodeHeader(p, available, payload_len, type, flags, stream_id)) {
        if (error) *error = "bad frame header";
        Clear();
        return ReadResult::Error;
    }

    if (payload_len > max_payload_) {
        if (error) *error = "frame too large";
        Clear();
        return ReadResult::Error;
    }

    const size_t total = 8 + static_cast<size_t>(payload_len);
    if (available < total) return ReadResult::NeedMore;

    out.type = type;
    out.flags = flags;
    out.stream_id = stream_id;
    out.payload.assign(p + 8, p + total);

    offset_ += total;

    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    } else if (offset_ > 64 * 1024 && offset_ * 2 > buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + offset_);
        offset_ = 0;
    }

    return ReadResult::Frame;
}

} // namespace wcs
