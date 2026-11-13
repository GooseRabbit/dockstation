#include <cstdlib>
#include <limits>
#include <iostream>

#include "hardware/interfaces/controller_protocol.hpp"

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

ControllerMessageEnvelope valid_message() {
    ControllerMessageEnvelope message{};
    message.type = ControllerMessageType::Hello;
    message.protocol_version = ControllerProtocolVersion{1U, 0U};
    message.controller_id = "controller-01";
    message.controller_instance_id = "boot-session-01";
    message.sequence_number = 0U;
    message.sent_at = "timestamp-not-yet-standardized";
    return message;
}
}  // namespace

int main() {
    auto message = valid_message();
    auto result = validate_controller_message(message);
    expect(result.status == ControllerMessageValidationStatus::Valid,
           "accepts a complete controller message envelope");
    expect(result.field.empty(), "valid result has no failing field");

    constexpr ControllerMessageType supported_types[] = {
        ControllerMessageType::Hello,
        ControllerMessageType::HelloAck,
        ControllerMessageType::Heartbeat,
        ControllerMessageType::RequestSnapshot,
        ControllerMessageType::TelemetrySnapshot,
        ControllerMessageType::Error};
    bool all_message_types_known = true;
    for (const auto type : supported_types) {
        all_message_types_known = all_message_types_known &&
                                  is_known_controller_message_type(type);
    }
    expect(all_message_types_known, "recognizes every defined controller message type");
    expect(!is_known_controller_message_type(ControllerMessageType::Unknown),
           "rejects the unknown message type");
    expect(!is_known_controller_message_type(static_cast<ControllerMessageType>(255U)),
           "rejects an out-of-range message type");

    message = valid_message();
    message.protocol_version.reset();
    result = validate_controller_message(message);
    expect(result.status == ControllerMessageValidationStatus::MissingProtocolVersion &&
               result.field == "protocol_version",
           "rejects a missing protocol version");

    message = valid_message();
    message.controller_id = {};
    result = validate_controller_message(message);
    expect(result.status == ControllerMessageValidationStatus::MissingControllerId &&
               result.field == "controller_id",
           "rejects an empty controller identifier");

    message = valid_message();
    message.controller_instance_id = {};
    result = validate_controller_message(message);
    expect(result.status == ControllerMessageValidationStatus::MissingControllerInstanceId &&
               result.field == "controller_instance_id",
           "rejects an empty controller instance identifier");

    message = valid_message();
    message.sequence_number.reset();
    result = validate_controller_message(message);
    expect(result.status == ControllerMessageValidationStatus::MissingSequenceNumber &&
               result.field == "sequence_number",
           "rejects a missing sequence number");

    message = valid_message();
    message.sent_at = {};
    result = validate_controller_message(message);
    expect(result.status == ControllerMessageValidationStatus::MissingTimestamp &&
               result.field == "sent_at",
           "rejects a missing timestamp");

    message = valid_message();
    message.sequence_number = 0U;
    expect(validate_controller_message(message).status ==
               ControllerMessageValidationStatus::Valid,
           "accepts sequence zero as a valid first sequence value");

    constexpr SensorQualityStatus supported_quality[] = {
        SensorQualityStatus::Ok,
        SensorQualityStatus::Stale,
        SensorQualityStatus::Invalid,
        SensorQualityStatus::Unavailable,
        SensorQualityStatus::NotConfigured,
        SensorQualityStatus::Unknown};
    bool all_quality_values_known = true;
    for (const auto status : supported_quality) {
        all_quality_values_known = all_quality_values_known &&
                                   is_known_sensor_quality_status(status);
    }
    expect(all_quality_values_known, "recognizes all six sensor quality statuses from docs/06");
    expect(!is_known_sensor_quality_status(static_cast<SensorQualityStatus>(255U)),
           "rejects an out-of-range sensor quality status");



    // The message kind itself is the content of these four read-only controls.
    constexpr ControllerMessageType empty_body_types[] = {
        ControllerMessageType::Hello,
        ControllerMessageType::HelloAck,
        ControllerMessageType::Heartbeat,
        ControllerMessageType::RequestSnapshot};
    bool all_empty_bodies_valid = true;
    for (const auto type : empty_body_types) {
        all_empty_bodies_valid = all_empty_bodies_valid &&
            validate_controller_message_payload(type, EmptyControllerMessagePayload{}).status ==
                ControllerPayloadValidationStatus::Valid;
    }
    expect(all_empty_bodies_valid,
           "accepts empty bodies for HELLO, HELLO_ACK, HEARTBEAT and REQUEST_SNAPSHOT");

    expect(validate_controller_message_payload(
               ControllerMessageType::Unknown, EmptyControllerMessagePayload{}).status ==
               ControllerPayloadValidationStatus::UnknownMessageType,
           "rejects an unknown message type when validating a body");
    expect(validate_controller_message_payload(
               ControllerMessageType::TelemetrySnapshot, EmptyControllerMessagePayload{}).status ==
               ControllerPayloadValidationStatus::PayloadTypeMismatch,
           "rejects an empty body for TELEMETRY_SNAPSHOT");
    expect(validate_controller_message_payload(
               ControllerMessageType::Hello, ControllerTelemetrySnapshotPayload{}).status ==
               ControllerPayloadValidationStatus::PayloadTypeMismatch,
           "rejects telemetry payload for HELLO");

    const ControllerTelemetryMeasurement measurements[] = {
        {"cpu.temperature", 42.5, "°C", SensorQualityStatus::Ok, "measurement-time"},
        {"lid.closed", true, "bool", SensorQualityStatus::Unknown, "measurement-time"},
        {"weather.mode", std::string_view{"AUTO"}, "state", SensorQualityStatus::Stale,
         "measurement-time"}};
    const ControllerTelemetrySnapshotPayload snapshot{
        measurements, sizeof(measurements) / sizeof(measurements[0])};
    const ControllerMessagePayload telemetry_payload{snapshot};
    auto payload_result = validate_controller_message_payload(
        ControllerMessageType::TelemetrySnapshot, telemetry_payload);
    expect(payload_result.status == ControllerPayloadValidationStatus::Valid,
           "accepts numeric, boolean and text sensor values with unit, quality and measurement time");

    const ControllerTelemetrySnapshotPayload empty_snapshot{};
    expect(validate_controller_message_payload(
               ControllerMessageType::TelemetrySnapshot, empty_snapshot).status ==
               ControllerPayloadValidationStatus::Valid,
           "accepts a structurally empty telemetry list without claiming sensor health");

    const ControllerTelemetrySnapshotPayload missing_storage{nullptr, 1U};
    expect(validate_controller_message_payload(
               ControllerMessageType::TelemetrySnapshot, missing_storage).status ==
               ControllerPayloadValidationStatus::MissingMeasurementStorage,
           "rejects a nonzero telemetry count without measurement storage");

    auto invalid_measurement = measurements[0];
    invalid_measurement.sensor_id = {};
    const ControllerTelemetryMeasurement missing_id[] = {invalid_measurement};
    expect(validate_controller_message_payload(
               ControllerMessageType::TelemetrySnapshot,
               ControllerTelemetrySnapshotPayload{missing_id, 1U}).status ==
               ControllerPayloadValidationStatus::MissingSensorId,
           "rejects telemetry without a sensor identifier");

    invalid_measurement = measurements[0];
    invalid_measurement.unit = {};
    const ControllerTelemetryMeasurement missing_unit[] = {invalid_measurement};
    expect(validate_controller_message_payload(
               ControllerMessageType::TelemetrySnapshot,
               ControllerTelemetrySnapshotPayload{missing_unit, 1U}).status ==
               ControllerPayloadValidationStatus::MissingMeasurementUnit,
           "rejects measurements without a unit");

    invalid_measurement = measurements[0];
    invalid_measurement.measured_at = {};
    const ControllerTelemetryMeasurement missing_measurement_time[] = {invalid_measurement};
    expect(validate_controller_message_payload(
               ControllerMessageType::TelemetrySnapshot,
               ControllerTelemetrySnapshotPayload{missing_measurement_time, 1U}).status ==
               ControllerPayloadValidationStatus::MissingMeasurementTimestamp,
           "rejects measurements without their own measurement timestamp");

    invalid_measurement = measurements[0];
    invalid_measurement.quality = static_cast<SensorQualityStatus>(255U);
    const ControllerTelemetryMeasurement unknown_quality[] = {invalid_measurement};
    expect(validate_controller_message_payload(
               ControllerMessageType::TelemetrySnapshot,
               ControllerTelemetrySnapshotPayload{unknown_quality, 1U}).status ==
               ControllerPayloadValidationStatus::UnknownSensorQuality,
           "rejects an out-of-range sensor quality value");

    invalid_measurement = measurements[0];
    invalid_measurement.value = std::numeric_limits<double>::quiet_NaN();
    const ControllerTelemetryMeasurement non_finite_value[] = {invalid_measurement};
    expect(validate_controller_message_payload(
               ControllerMessageType::TelemetrySnapshot,
               ControllerTelemetrySnapshotPayload{non_finite_value, 1U}).status ==
               ControllerPayloadValidationStatus::NonFiniteMeasurementValue,
           "rejects a non-finite numeric sensor value");

    const ControllerErrorPayload error_payload{
        "SENSOR_READ_FAILED",
        ControllerErrorSeverity::Warning,
        "The temperature sensor did not respond",
        {}};
    payload_result = validate_controller_message_payload(
        ControllerMessageType::Error, error_payload);
    expect(payload_result.status == ControllerPayloadValidationStatus::Valid,
           "accepts a complete ERROR body when the cause is unknown");
    expect(validate_controller_message_payload(
               ControllerMessageType::TelemetrySnapshot, error_payload).status ==
               ControllerPayloadValidationStatus::PayloadTypeMismatch,
           "rejects an ERROR payload for TELEMETRY_SNAPSHOT");

    auto invalid_error = error_payload;
    invalid_error.error_code = {};
    expect(validate_controller_message_payload(
               ControllerMessageType::Error, invalid_error).status ==
               ControllerPayloadValidationStatus::MissingErrorCode,
           "rejects ERROR without an error code");

    invalid_error = error_payload;
    invalid_error.severity = static_cast<ControllerErrorSeverity>(255U);
    expect(validate_controller_message_payload(
               ControllerMessageType::Error, invalid_error).status ==
               ControllerPayloadValidationStatus::UnknownErrorSeverity,
           "rejects an unknown ERROR severity");

    invalid_error = error_payload;
    invalid_error.description = {};
    expect(validate_controller_message_payload(
               ControllerMessageType::Error, invalid_error).status ==
               ControllerPayloadValidationStatus::MissingErrorDescription,
           "rejects ERROR without a description");

    expect(!is_known_controller_error_severity(ControllerErrorSeverity::Unknown) &&
               is_known_controller_error_severity(ControllerErrorSeverity::Critical),
           "recognizes only the defined ERROR severity values");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
