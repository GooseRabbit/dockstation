#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

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


// The initial read-only exchange has no extra body fields for HELLO,
// HELLO_ACK, HEARTBEAT or REQUEST_SNAPSHOT. The message type itself identifies
// the handshake acknowledgement and snapshot request.
struct EmptyControllerMessagePayload {};

using ControllerSensorValue =
    std::variant<bool, std::int64_t, std::uint64_t, double, std::string_view>;

// One measurement in a telemetry snapshot. All string views are non-owning;
// the caller must keep their backing storage alive while the message is used.
// measured_at is required but its textual format/time standard is not selected.
struct ControllerTelemetryMeasurement {
    std::string_view sensor_id{};
    ControllerSensorValue value{};
    std::string_view unit{};
    SensorQualityStatus quality{SensorQualityStatus::Unknown};
    std::string_view measured_at{};
};

// The array and all strings/values are caller-owned. A zero-count snapshot is
// permitted, but it does not imply that any unreported sensor is healthy.
struct ControllerTelemetrySnapshotPayload {
    const ControllerTelemetryMeasurement* measurements{nullptr};
    std::size_t measurement_count{0U};
};

enum class ControllerErrorSeverity : std::uint8_t {
    Unknown,
    Info,
    Warning,
    Error,
    Critical
};

struct ControllerErrorPayload {
    std::string_view error_code{};
    ControllerErrorSeverity severity{ControllerErrorSeverity::Unknown};
    std::string_view description{};
    // Empty means that the cause is not known; do not invent a reason.
    std::string_view reason_text{};
};

using ControllerMessagePayload = std::variant<
    EmptyControllerMessagePayload,
    ControllerTelemetrySnapshotPayload,
    ControllerErrorPayload>;

enum class ControllerPayloadValidationStatus : std::uint8_t {
    Valid,
    UnknownMessageType,
    PayloadTypeMismatch,
    MissingMeasurementStorage,
    MissingSensorId,
    MissingMeasurementUnit,
    MissingMeasurementTimestamp,
    UnknownSensorQuality,
    NonFiniteMeasurementValue,
    MissingErrorCode,
    UnknownErrorSeverity,
    MissingErrorDescription
};

struct ControllerPayloadValidationResult {
    ControllerPayloadValidationStatus status{
        ControllerPayloadValidationStatus::PayloadTypeMismatch};
    std::string_view field{};
};

[[nodiscard]] constexpr bool is_known_controller_error_severity(
    ControllerErrorSeverity severity) noexcept {
    switch (severity) {
        case ControllerErrorSeverity::Info:
        case ControllerErrorSeverity::Warning:
        case ControllerErrorSeverity::Error:
        case ControllerErrorSeverity::Critical:
            return true;
        case ControllerErrorSeverity::Unknown:
            return false;
    }
    return false;
}

// Validates the typed logical body for a message. This is deliberately
// separate from envelope validation and does not encode or decode wire bytes.
[[nodiscard]] inline ControllerPayloadValidationResult
validate_controller_message_payload(
    ControllerMessageType type,
    const ControllerMessagePayload& payload) noexcept {
    if (!is_known_controller_message_type(type)) {
        return {ControllerPayloadValidationStatus::UnknownMessageType, "type"};
    }

    switch (type) {
        case ControllerMessageType::Hello:
        case ControllerMessageType::HelloAck:
        case ControllerMessageType::Heartbeat:
        case ControllerMessageType::RequestSnapshot:
            return std::holds_alternative<EmptyControllerMessagePayload>(payload)
                ? ControllerPayloadValidationResult{ControllerPayloadValidationStatus::Valid, {}}
                : ControllerPayloadValidationResult{ControllerPayloadValidationStatus::PayloadTypeMismatch, "payload"};

        case ControllerMessageType::TelemetrySnapshot: {
            const auto* snapshot = std::get_if<ControllerTelemetrySnapshotPayload>(&payload);
            if (snapshot == nullptr) {
                return {ControllerPayloadValidationStatus::PayloadTypeMismatch, "payload"};
            }
            if (snapshot->measurement_count > 0U && snapshot->measurements == nullptr) {
                return {ControllerPayloadValidationStatus::MissingMeasurementStorage, "measurements"};
            }
            for (std::size_t index = 0U; index < snapshot->measurement_count; ++index) {
                const auto& measurement = snapshot->measurements[index];
                if (measurement.sensor_id.empty()) {
                    return {ControllerPayloadValidationStatus::MissingSensorId, "sensor_id"};
                }
                if (measurement.unit.empty()) {
                    return {ControllerPayloadValidationStatus::MissingMeasurementUnit, "unit"};
                }
                if (measurement.measured_at.empty()) {
                    return {ControllerPayloadValidationStatus::MissingMeasurementTimestamp,
                            "measured_at"};
                }
                if (!is_known_sensor_quality_status(measurement.quality)) {
                    return {ControllerPayloadValidationStatus::UnknownSensorQuality, "quality"};
                }
                const auto* numeric_value = std::get_if<double>(&measurement.value);
                if (numeric_value != nullptr && !std::isfinite(*numeric_value)) {
                    return {ControllerPayloadValidationStatus::NonFiniteMeasurementValue,
                            "value"};
                }
            }
            return {ControllerPayloadValidationStatus::Valid, {}};
        }

        case ControllerMessageType::Error: {
            const auto* error = std::get_if<ControllerErrorPayload>(&payload);
            if (error == nullptr) {
                return {ControllerPayloadValidationStatus::PayloadTypeMismatch, "payload"};
            }
            if (error->error_code.empty()) {
                return {ControllerPayloadValidationStatus::MissingErrorCode, "error_code"};
            }
            if (!is_known_controller_error_severity(error->severity)) {
                return {ControllerPayloadValidationStatus::UnknownErrorSeverity, "severity"};
            }
            if (error->description.empty()) {
                return {ControllerPayloadValidationStatus::MissingErrorDescription, "description"};
            }
            return {ControllerPayloadValidationStatus::Valid, {}};
        }

        case ControllerMessageType::Unknown:
            break;
    }
    return {ControllerPayloadValidationStatus::UnknownMessageType, "type"};
}

}  // namespace dockstation::hardware::interfaces
