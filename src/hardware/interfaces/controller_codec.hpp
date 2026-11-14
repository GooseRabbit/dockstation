#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "hardware/interfaces/controller_protocol.hpp"

namespace dockstation::hardware::interfaces {

// Wire framing constants are independent of UART, USB and ESP-IDF.
inline constexpr std::uint8_t kControllerCodecMagicFirst = 0x44U;  // 'D'
inline constexpr std::uint8_t kControllerCodecMagicSecond = 0x53U; // 'S'
inline constexpr std::uint8_t kControllerCodecWireVersion = 1U;
inline constexpr std::size_t kControllerCodecMaximumFrameSize = 2048U;
inline constexpr std::size_t kControllerCodecMaximumMeasurements = 16U;

// Validates a wire text field: well-formed UTF-8 without embedded NUL.
// Empty strings are valid at this low level; individual fields may be required.
[[nodiscard]] bool is_valid_controller_wire_text(std::string_view value) noexcept;

enum class ControllerCodecStatus : std::uint8_t {
    Success,
    NullBuffer,
    BufferTooSmall,
    FrameTooLarge,
    InvalidFrameSize,
    BadMagic,
    UnsupportedWireVersion,
    UnknownMessageType,
    PayloadLengthMismatch,
    CrcMismatch,
    InvalidEnvelope,
    InvalidPayload,
    MalformedField,
    InvalidUtf8,
    StringTooLong,
    TooManyMeasurements,
    UnsupportedValueType,
    InvalidBooleanValue
};

struct ControllerCodecResult {
    ControllerCodecStatus status{ControllerCodecStatus::MalformedField};
    std::size_t frame_size{0U};
    std::size_t error_offset{0U};
};

// Decoded text fields refer to the caller-owned input frame. Decoded telemetry
// measurements live in the caller-owned storage below. Both must outlive use
// of the decoded message. No dynamic allocation is performed by the codec.
struct ControllerCodecDecodeStorage {
    std::array<ControllerTelemetryMeasurement, kControllerCodecMaximumMeasurements>
        measurements{};
};

struct ControllerCodecDecodedMessage {
    ControllerMessageEnvelope envelope{};
    ControllerMessagePayload payload{EmptyControllerMessagePayload{}};
};

// Encodes one complete frame:
// magic[2], wire_version[1], message_type[1], body_length_le[2],
// body[body_length], crc32_le[4]. CRC-32/ISO-HDLC covers header and body.
[[nodiscard]] ControllerCodecResult encode_controller_message(
    const ControllerMessageEnvelope& envelope,
    const ControllerMessagePayload& payload,
    std::uint8_t* destination,
    std::size_t destination_capacity) noexcept;

// Decodes exactly one complete frame. The input buffer must remain alive while
// the decoded envelope and any string-valued fields are in use.
[[nodiscard]] ControllerCodecResult decode_controller_message(
    const std::uint8_t* frame,
    std::size_t frame_size,
    ControllerCodecDecodeStorage& storage,
    ControllerCodecDecodedMessage& decoded) noexcept;

}  // namespace dockstation::hardware::interfaces
