#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace dockstation::hardware::interfaces {

// Logical message kinds for the initial read-only Repka <-> ESP32 exchange.
// This enum does not define a wire representation or a physical transport.
enum class ControllerMessageType : std::uint8_t {
    Unknown,
    Hello,
    HelloAck,
    Heartbeat,
    RequestSnapshot,
    TelemetrySnapshot,
    Error
};

struct ControllerProtocolVersion {
    std::uint16_t major{0U};
    std::uint16_t minor{0U};
};

// Transport-independent metadata shared by controller messages. String views
// are non-owning and must remain valid for the duration of validation/use.
// The timestamp format, version compatibility policy and sequence ordering
// rules are deliberately left to later protocol/bridge decisions.
struct ControllerMessageEnvelope {
    ControllerMessageType type{ControllerMessageType::Unknown};
    std::optional<ControllerProtocolVersion> protocol_version{};
    std::string_view controller_id{};
    std::string_view controller_instance_id{};
    std::optional<std::uint64_t> sequence_number{};
    std::string_view sent_at{};
};

// Quality values for sensor telemetry, aligned with docs/06 §3 and §25.1.
// ERROR is intentionally not a sensor quality: it is a message/processing error.
enum class SensorQualityStatus : std::uint8_t {
    Ok,
    Stale,
    Invalid,
    Unavailable,
    NotConfigured,
    Unknown
};

enum class ControllerMessageValidationStatus : std::uint8_t {
    Valid,
    UnknownMessageType,
    MissingProtocolVersion,
    MissingControllerId,
    MissingControllerInstanceId,
    MissingSequenceNumber,
    MissingTimestamp
};

struct ControllerMessageValidationResult {
    ControllerMessageValidationStatus status{ControllerMessageValidationStatus::UnknownMessageType};
    std::string_view field{};
};

[[nodiscard]] constexpr bool is_known_controller_message_type(
    ControllerMessageType type) noexcept {
    switch (type) {
        case ControllerMessageType::Hello:
        case ControllerMessageType::HelloAck:
        case ControllerMessageType::Heartbeat:
        case ControllerMessageType::RequestSnapshot:
        case ControllerMessageType::TelemetrySnapshot:
        case ControllerMessageType::Error:
            return true;
        case ControllerMessageType::Unknown:
            return false;
    }
    return false;
}

[[nodiscard]] constexpr bool is_known_sensor_quality_status(
    SensorQualityStatus status) noexcept {
    switch (status) {
        case SensorQualityStatus::Ok:
        case SensorQualityStatus::Stale:
        case SensorQualityStatus::Invalid:
        case SensorQualityStatus::Unavailable:
        case SensorQualityStatus::NotConfigured:
        case SensorQualityStatus::Unknown:
            return true;
    }
    return false;
}

// Validates only the common logical envelope. Message-specific payload
// validation, version negotiation, duplicate/order tracking and wire decoding
// are separate responsibilities and are not implemented by this function.
[[nodiscard]] inline ControllerMessageValidationResult validate_controller_message(
    const ControllerMessageEnvelope& message) noexcept {
    if (!is_known_controller_message_type(message.type)) {
        return {ControllerMessageValidationStatus::UnknownMessageType, "type"};
    }
    if (!message.protocol_version.has_value()) {
        return {ControllerMessageValidationStatus::MissingProtocolVersion,
                "protocol_version"};
    }
    if (message.controller_id.empty()) {
        return {ControllerMessageValidationStatus::MissingControllerId, "controller_id"};
    }
    if (message.controller_instance_id.empty()) {
        return {ControllerMessageValidationStatus::MissingControllerInstanceId,
                "controller_instance_id"};
    }
    if (!message.sequence_number.has_value()) {
        return {ControllerMessageValidationStatus::MissingSequenceNumber, "sequence_number"};
    }
    if (message.sent_at.empty()) {
        return {ControllerMessageValidationStatus::MissingTimestamp, "sent_at"};
    }
    return {ControllerMessageValidationStatus::Valid, {}};
}

}  // namespace dockstation::hardware::interfaces
