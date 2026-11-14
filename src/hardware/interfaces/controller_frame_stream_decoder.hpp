#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "hardware/interfaces/controller_codec.hpp"

namespace dockstation::hardware::interfaces {

enum class ControllerFrameStreamStatus : std::uint8_t {
    NeedMoreData,
    FrameReady,
    FramePending,
    NullInput,
    BufferLimitExceeded,
    NoFrameAvailable
};

struct ControllerFrameStreamResult {
    ControllerFrameStreamStatus status{
        ControllerFrameStreamStatus::NeedMoreData};
    // Number of bytes consumed from the input buffer. The caller must retain
    // and resubmit any remaining suffix when FrameReady is returned.
    std::size_t bytes_consumed{0U};
    std::size_t frame_size{0U};
    // Number of noise/invalid-prefix bytes discarded during this call.
    std::size_t discarded_bytes{0U};
};

// Incrementally finds complete ControllerCodec frames in a byte stream.
// It owns one bounded frame buffer and performs no dynamic allocation.
// It checks the outer signature/version/type/length only; the caller must
// pass a ready frame to decode_controller_message() for CRC and body checks.
class ControllerFrameStreamDecoder final {
public:
    [[nodiscard]] ControllerFrameStreamResult push_bytes(
        const std::uint8_t* bytes, std::size_t byte_count) noexcept;

    [[nodiscard]] bool has_frame() const noexcept { return frame_ready_; }

    // Valid only while has_frame() is true. The view is invalidated when the
    // frame is consumed/rejected, the decoder is reset, or new bytes mutate it.
    [[nodiscard]] const std::uint8_t* frame_data() const noexcept;

    [[nodiscard]] std::size_t frame_size() const noexcept {
        return frame_ready_ ? frame_size_ : 0U;
    }

    // Accept a frame after decode_controller_message() succeeds.
    // Any already-buffered trailing bytes are retained and scanned.
    [[nodiscard]] bool consume_frame() noexcept;

    // Reject a candidate (for example, on CRC failure), drop one leading byte,
    // and rescan the remaining bytes so a later frame can be recovered.
    [[nodiscard]] bool reject_frame() noexcept;

    void reset() noexcept;

    [[nodiscard]] std::size_t buffered_size() const noexcept {
        return buffered_size_;
    }

private:
    [[nodiscard]] bool scan_for_frame(
        std::size_t& discarded_bytes) noexcept;
    void discard_prefix(std::size_t count) noexcept;

    std::array<std::uint8_t, kControllerCodecMaximumFrameSize> buffer_{};
    std::size_t buffered_size_{0U};
    std::size_t frame_size_{0U};
    bool frame_ready_{false};
};

}  // namespace dockstation::hardware::interfaces
