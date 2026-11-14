#include "hardware/interfaces/controller_frame_stream_decoder.hpp"

#include <cstring>

namespace dockstation::hardware::interfaces {
namespace {

[[nodiscard]] bool is_known_wire_message_type(std::uint8_t type) noexcept {
    return type >= 1U && type <= 6U;
}

[[nodiscard]] std::size_t read_u16_le(
    const std::uint8_t* data) noexcept {
    return static_cast<std::size_t>(data[0]) |
           (static_cast<std::size_t>(data[1]) << 8U);
}

}  // namespace

ControllerFrameStreamResult ControllerFrameStreamDecoder::push_bytes(
    const std::uint8_t* bytes, std::size_t byte_count) noexcept {
    if (frame_ready_) {
        return {ControllerFrameStreamStatus::FramePending, 0U, frame_size_, 0U};
    }
    if (byte_count > 0U && bytes == nullptr) {
        return {ControllerFrameStreamStatus::NullInput, 0U, 0U, 0U};
    }

    ControllerFrameStreamResult result{};
    for (std::size_t index = 0U; index < byte_count; ++index) {
        if (buffered_size_ >= buffer_.size()) {
            // A complete in-range candidate would already have been reported.
            // If no candidate exists, discard one byte and continue resync.
            discard_prefix(1U);
            ++result.discarded_bytes;
        }

        buffer_[buffered_size_] = bytes[index];
        ++buffered_size_;
        ++result.bytes_consumed;

        if (scan_for_frame(result.discarded_bytes)) {
            result.status = ControllerFrameStreamStatus::FrameReady;
            result.frame_size = frame_size_;
            return result;
        }
    }

    result.status = ControllerFrameStreamStatus::NeedMoreData;
    return result;
}

const std::uint8_t* ControllerFrameStreamDecoder::frame_data() const noexcept {
    return frame_ready_ ? buffer_.data() : nullptr;
}

bool ControllerFrameStreamDecoder::consume_frame() noexcept {
    if (!frame_ready_) {
        return false;
    }

    const auto consumed_size = frame_size_;
    frame_ready_ = false;
    frame_size_ = 0U;
    discard_prefix(consumed_size);

    std::size_t ignored_discarded_bytes = 0U;
    static_cast<void>(scan_for_frame(ignored_discarded_bytes));
    return true;
}

bool ControllerFrameStreamDecoder::reject_frame() noexcept {
    if (!frame_ready_) {
        return false;
    }

    frame_ready_ = false;
    frame_size_ = 0U;
    discard_prefix(1U);

    std::size_t ignored_discarded_bytes = 0U;
    static_cast<void>(scan_for_frame(ignored_discarded_bytes));
    return true;
}

void ControllerFrameStreamDecoder::reset() noexcept {
    buffered_size_ = 0U;
    frame_size_ = 0U;
    frame_ready_ = false;
}

bool ControllerFrameStreamDecoder::scan_for_frame(
    std::size_t& discarded_bytes) noexcept {
    while (buffered_size_ > 0U) {
        if (buffer_[0] != kControllerCodecMagicFirst) {
            discard_prefix(1U);
            ++discarded_bytes;
            continue;
        }
        if (buffered_size_ < 2U) {
            return false;
        }
        if (buffer_[1] != kControllerCodecMagicSecond) {
            discard_prefix(1U);
            ++discarded_bytes;
            continue;
        }
        if (buffered_size_ < 3U) {
            return false;
        }
        if (buffer_[2] != kControllerCodecWireVersion) {
            discard_prefix(1U);
            ++discarded_bytes;
            continue;
        }
        if (buffered_size_ < 4U) {
            return false;
        }
        if (!is_known_wire_message_type(buffer_[3])) {
            discard_prefix(1U);
            ++discarded_bytes;
            continue;
        }
        if (buffered_size_ < 6U) {
            return false;
        }

        const auto body_size = read_u16_le(buffer_.data() + 4U);
        const auto total_size = 6U + body_size + 4U;
        if (total_size > buffer_.size()) {
            discard_prefix(1U);
            ++discarded_bytes;
            continue;
        }
        if (buffered_size_ < total_size) {
            return false;
        }

        frame_size_ = total_size;
        frame_ready_ = true;
        return true;
    }
    return false;
}

void ControllerFrameStreamDecoder::discard_prefix(std::size_t count) noexcept {
    if (count >= buffered_size_) {
        buffered_size_ = 0U;
        return;
    }

    const auto remaining = buffered_size_ - count;
    std::memmove(buffer_.data(), buffer_.data() + count, remaining);
    buffered_size_ = remaining;
}

}  // namespace dockstation::hardware::interfaces
