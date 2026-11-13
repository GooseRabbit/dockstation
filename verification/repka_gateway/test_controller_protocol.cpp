#include <cstdlib>
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

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
