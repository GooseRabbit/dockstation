#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "hardware/interfaces/controller_frame_stream_decoder.hpp"

using namespace dockstation::hardware::interfaces;

namespace {
int failures = 0;
int checks = 0;

void expect(bool condition, const char* name) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << name << '\n';
    } else {
        std::cerr << "FAIL - " << name << '\n';
        ++failures;
    }
}

ControllerMessageEnvelope hello_envelope() {
    ControllerMessageEnvelope envelope{};
    envelope.type = ControllerMessageType::Hello;
    envelope.protocol_version = ControllerProtocolVersion{1U, 0U};
    envelope.controller_id = "esp32-01";
    envelope.controller_instance_id = "boot-01";
    envelope.sequence_number = 1U;
    envelope.sent_at = "2026-11-14T18:00:00Z";
    return envelope;
}

std::size_t make_hello_frame(
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize>& frame) {
    const auto result = encode_controller_message(
        hello_envelope(),
        ControllerMessagePayload{EmptyControllerMessagePayload{}},
        frame.data(), frame.size());
    return result.status == ControllerCodecStatus::Success
        ? result.frame_size
        : 0U;
}

ControllerCodecResult decode_ready_frame(
    const ControllerFrameStreamDecoder& stream,
    ControllerCodecDecodeStorage& storage,
    ControllerCodecDecodedMessage& decoded) {
    return decode_controller_message(
        stream.frame_data(), stream.frame_size(), storage, decoded);
}
}  // namespace

int main() {
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize> frame{};
    const auto frame_size = make_hello_frame(frame);
    expect(frame_size > 0U, "builds a valid input frame using ControllerCodec");

    // Feed one byte at a time; a frame must be reported only when complete.
    ControllerFrameStreamDecoder byte_stream{};
    bool bytewise_ready = false;
    for (std::size_t index = 0U; index < frame_size; ++index) {
        const auto result = byte_stream.push_bytes(frame.data() + index, 1U);
        if (index + 1U < frame_size) {
            expect(result.status == ControllerFrameStreamStatus::NeedMoreData,
                   "waits for the remainder of an incomplete frame");
        } else {
            bytewise_ready =
                result.status == ControllerFrameStreamStatus::FrameReady &&
                result.bytes_consumed == 1U &&
                result.frame_size == frame_size;
        }
    }
    expect(bytewise_ready, "reports FrameReady after the final byte arrives");
    expect(byte_stream.has_frame() &&
               byte_stream.frame_data() != nullptr &&
               byte_stream.frame_size() == frame_size,
           "exposes the complete candidate frame while it is pending");

    ControllerCodecDecodeStorage storage{};
    ControllerCodecDecodedMessage decoded{};
    auto decode_result = decode_ready_frame(byte_stream, storage, decoded);
    expect(decode_result.status == ControllerCodecStatus::Success &&
               decoded.envelope.controller_id == "esp32-01",
           "passes a bytewise-assembled frame to ControllerCodec successfully");

    const std::uint8_t extra_byte = 0xAAU;
    const auto pending_result = byte_stream.push_bytes(&extra_byte, 1U);
    expect(pending_result.status == ControllerFrameStreamStatus::FramePending &&
               pending_result.bytes_consumed == 0U,
           "does not overwrite a frame that the caller has not consumed");
    expect(byte_stream.consume_frame() && !byte_stream.has_frame() &&
               byte_stream.buffered_size() == 0U,
           "consumes a successfully decoded frame and clears its storage");
    expect(!byte_stream.consume_frame() && !byte_stream.reject_frame(),
           "refuses consume/reject operations when no frame is pending");

    // Split the two-byte signature across calls, with preceding noise.
    ControllerFrameStreamDecoder split_magic_stream{};
    const std::array<std::uint8_t, 3U> noise_and_first_magic{0x00U, 0x7FU, 0x44U};
    auto result = split_magic_stream.push_bytes(
        noise_and_first_magic.data(), noise_and_first_magic.size());
    expect(result.status == ControllerFrameStreamStatus::NeedMoreData &&
               result.discarded_bytes == 2U &&
               split_magic_stream.buffered_size() == 1U,
           "discards noise but preserves a possible first magic byte");
    result = split_magic_stream.push_bytes(
        frame.data() + 1U, frame_size - 1U);
    expect(result.status == ControllerFrameStreamStatus::FrameReady &&
               result.frame_size == frame_size,
           "completes a frame when its DS signature was split across input chunks");
    expect(decode_ready_frame(split_magic_stream, storage, decoded).status ==
               ControllerCodecStatus::Success,
           "decodes a frame recovered after leading noise");
    expect(split_magic_stream.consume_frame(), "consumes frame recovered after noise");

    // Invalid outer headers must not block a later valid frame.
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize + 32U> noisy_input{};
    const std::array<std::uint8_t, 10U> invalid_prefix{
        0x00U, 0x44U, 0x00U, 0x99U,
        0x44U, 0x53U, 0x02U, 0x01U, 0x00U, 0x00U};
    for (std::size_t index = 0U; index < invalid_prefix.size(); ++index) {
        noisy_input[index] = invalid_prefix[index];
    }
    for (std::size_t index = 0U; index < frame_size; ++index) {
        noisy_input[invalid_prefix.size() + index] = frame[index];
    }
    ControllerFrameStreamDecoder resync_stream{};
    result = resync_stream.push_bytes(
        noisy_input.data(), invalid_prefix.size() + frame_size);
    expect(result.status == ControllerFrameStreamStatus::FrameReady &&
               result.discarded_bytes >= invalid_prefix.size() &&
               result.frame_size == frame_size,
           "resynchronizes after noise, invalid magic and unsupported wire version");
    expect(decode_ready_frame(resync_stream, storage, decoded).status ==
               ControllerCodecStatus::Success,
           "recovers the valid frame after malformed headers");
    expect(resync_stream.consume_frame(), "consumes frame after malformed headers");

    // A declared length above the fixed bound must be discarded before buffering
    // the alleged body; the following valid frame must still be found.
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize + 16U> oversized_header{};
    oversized_header[0] = kControllerCodecMagicFirst;
    oversized_header[1] = kControllerCodecMagicSecond;
    oversized_header[2] = kControllerCodecWireVersion;
    oversized_header[3] = 1U;
    oversized_header[4] = 0xFFU;
    oversized_header[5] = 0xFFU;
    for (std::size_t index = 0U; index < frame_size; ++index) {
        oversized_header[6U + index] = frame[index];
    }
    ControllerFrameStreamDecoder bounded_stream{};
    result = bounded_stream.push_bytes(
        oversized_header.data(), 6U + frame_size);
    expect(result.status == ControllerFrameStreamStatus::FrameReady &&
               result.frame_size == frame_size &&
               result.discarded_bytes >= 6U,
           "rejects an impossible advertised length and finds the next valid frame");
    expect(decode_ready_frame(bounded_stream, storage, decoded).status ==
               ControllerCodecStatus::Success,
           "decodes the frame following an oversized header");

    // Two frames in one caller buffer are consumed in order. The stream decoder
    // stops at the first boundary and reports exactly how much input it used.
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize> second_frame{};
    const auto second_size = make_hello_frame(second_frame);
    std::array<std::uint8_t, 2U * kControllerCodecMaximumFrameSize> joined{};
    for (std::size_t index = 0U; index < frame_size; ++index) {
        joined[index] = frame[index];
    }
    for (std::size_t index = 0U; index < second_size; ++index) {
        joined[frame_size + index] = second_frame[index];
    }
    ControllerFrameStreamDecoder paired_stream{};
    result = paired_stream.push_bytes(joined.data(), frame_size + second_size);
    expect(result.status == ControllerFrameStreamStatus::FrameReady &&
               result.bytes_consumed == frame_size,
           "stops at the first frame and reports the unconsumed input suffix");
    expect(paired_stream.consume_frame(), "consumes first frame from a concatenated input");
    result = paired_stream.push_bytes(
        joined.data() + frame_size, second_size);
    expect(result.status == ControllerFrameStreamStatus::FrameReady &&
               result.bytes_consumed == second_size,
           "accepts the caller's remaining suffix as the next frame");
    expect(decode_ready_frame(paired_stream, storage, decoded).status ==
               ControllerCodecStatus::Success,
           "decodes the second frame in concatenated input");

    // CRC/body validation stays in ControllerCodec. A bad candidate can be
    // rejected by the stream layer, which then resumes scanning later bytes.
    auto damaged_frame = frame;
    damaged_frame[frame_size - 1U] ^= 0x80U;
    ControllerFrameStreamDecoder damaged_stream{};
    result = damaged_stream.push_bytes(damaged_frame.data(), frame_size);
    expect(result.status == ControllerFrameStreamStatus::FrameReady,
           "extracts a complete candidate without treating framing as validation");
    expect(decode_ready_frame(damaged_stream, storage, decoded).status ==
               ControllerCodecStatus::CrcMismatch,
           "leaves CRC verification to ControllerCodec");
    expect(damaged_stream.reject_frame() && !damaged_stream.has_frame(),
           "allows caller to reject a CRC-failed candidate");

    result = damaged_stream.push_bytes(frame.data(), frame_size);
    expect(result.status == ControllerFrameStreamStatus::FrameReady &&
               decode_ready_frame(damaged_stream, storage, decoded).status ==
                   ControllerCodecStatus::Success,
           "recovers a valid frame after the caller rejects a damaged candidate");
    expect(damaged_stream.consume_frame(), "consumes frame after CRC rejection");

    // A short advertised length deliberately creates a false early candidate.
    // Rejecting it must allow the caller to feed the untouched remainder.
    auto wrong_length_frame = frame;
    wrong_length_frame[4] = 0x00U;
    wrong_length_frame[5] = 0x00U;
    ControllerFrameStreamDecoder length_recovery_stream{};
    result = length_recovery_stream.push_bytes(
        wrong_length_frame.data(), frame_size);
    expect(result.status == ControllerFrameStreamStatus::FrameReady &&
               length_recovery_stream.frame_size() == 10U,
           "uses the advertised body length to identify a candidate boundary");
    expect(decode_ready_frame(length_recovery_stream, storage, decoded).status !=
               ControllerCodecStatus::Success,
           "lets ControllerCodec reject a candidate with a corrupted length field");
    expect(length_recovery_stream.reject_frame(),
           "drops one byte from the invalid-length candidate to resynchronize");
    result = length_recovery_stream.push_bytes(
        wrong_length_frame.data() + result.bytes_consumed,
        frame_size - result.bytes_consumed);
    expect(result.status == ControllerFrameStreamStatus::NeedMoreData,
           "does not invent a frame from the remaining damaged payload");

    result = length_recovery_stream.push_bytes(frame.data(), frame_size);
    expect(result.status == ControllerFrameStreamStatus::FrameReady &&
               decode_ready_frame(length_recovery_stream, storage, decoded).status ==
                   ControllerCodecStatus::Success,
           "finds a clean frame after malformed length recovery");

    expect(length_recovery_stream.consume_frame(),
           "consumes clean frame after malformed length recovery");

    ControllerFrameStreamDecoder null_stream{};
    expect(null_stream.push_bytes(nullptr, 1U).status ==
               ControllerFrameStreamStatus::NullInput,
           "rejects a null input pointer when byte count is nonzero");
    expect(null_stream.push_bytes(nullptr, 0U).status ==
               ControllerFrameStreamStatus::NeedMoreData,
           "accepts an empty poll without inventing input bytes");
    null_stream.reset();
    expect(null_stream.buffered_size() == 0U && !null_stream.has_frame(),
           "reset clears a partial candidate and its pending state");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
