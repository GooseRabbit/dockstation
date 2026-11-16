#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

#include "communications/controller_subsystem_status_event_projector.hpp"
#include "hardware/interfaces/controller_protocol.hpp"

using namespace dockstation::hardware::interfaces;
using namespace dockstation::communications;

namespace {
int checks = 0;
int failures = 0;

void expect(bool condition, const char* name) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << name << '\n';
    } else {
        std::cerr << "FAIL - " << name << '\n';
        ++failures;
    }
}

ControllerCodecDecodedMessage error_message(
    const ControllerErrorPayload& payload) noexcept {
    ControllerCodecDecodedMessage message{};
    message.envelope.type = ControllerMessageType::Error;
    message.payload = payload;
    return message;
}
}  // namespace

int main() {
    ControllerSubsystemStatusEventProjector projector{};
    ControllerEventProjection output{};

    ControllerCodecDecodedMessage telemetry{};
    telemetry.envelope.type = ControllerMessageType::TelemetrySnapshot;
    expect(projector.project(telemetry, output) ==
               ControllerEventProjectionStatus::NotApplicable &&
               output.event_type.empty() && output.payload.empty(),
           "telemetry stays outside the SSM event projection path");

    const ControllerErrorPayload generic_error{
        "SENSOR_READ_FAILED",
        ControllerErrorSeverity::Warning,
        "Sensor read failed",
        {}};
    output = {};
    expect(projector.project(error_message(generic_error), output) ==
               ControllerEventProjectionStatus::NotApplicable &&
               output.event_type.empty() && output.payload.empty(),
           "generic ERROR without explicit subsystem state is not projected");

    const ControllerErrorPayload subsystem_error{
        "DHT22_READ_FAILED",
        ControllerErrorSeverity::Warning,
        "DHT22 read failed",
        "I2C timeout",
        std::string_view{"weather.temperature"},
        ControllerSubsystemStatus::Degraded};
    output = {};
    expect(projector.project(error_message(subsystem_error), output) ==
               ControllerEventProjectionStatus::Projected &&
               output.event_type == "SUBSYSTEM_STATUS_UPDATED" &&
               output.quality_status == "VALID" &&
               output.schema_version == "1" &&
               output.reason_code == std::string_view{"DHT22_READ_FAILED"} &&
               output.reason_text == std::string_view{"I2C timeout"},
           "explicit subsystem ID and status produce a normalized SSM event");
    expect(output.payload.find("\"subsystem_id\":\"weather.temperature\"") !=
               std::string_view::npos &&
               output.payload.find("\"status\":\"DEGRADED\"") !=
               std::string_view::npos &&
               output.payload.find("\"severity\":\"WARNING\"") !=
               std::string_view::npos,
           "event payload preserves subsystem ID, reported status and independent severity");

    dockstation::core::Event decoded_event{};
    decoded_event.event_type = output.event_type;
    decoded_event.payload = output.payload;
    decoded_event.quality_status = output.quality_status;
    decoded_event.schema_version = output.schema_version;
    decoded_event.reason_code = output.reason_code;
    decoded_event.reason_text = output.reason_text;
    const auto decoded_report = projector.decode(decoded_event);
    expect(decoded_report.status ==
               dockstation::core::SubsystemStatusDecodeStatus::Decoded &&
               decoded_report.report.subsystem_id == "weather.temperature" &&
               decoded_report.report.status ==
                   dockstation::core::ReportedSubsystemStatus::Degraded &&
               decoded_report.report.error_code == "DHT22_READ_FAILED" &&
               decoded_report.report.severity == "WARNING" &&
               decoded_report.report.reason_text ==
                   std::optional<std::string_view>{"I2C timeout"},
           "decodes typed status from the same validated ControllerLink frame without reparsing JSON");

    dockstation::core::Event altered_event = decoded_event;
    altered_event.payload = std::string_view{"{}"};
    expect(projector.decode(altered_event).status ==
               dockstation::core::SubsystemStatusDecodeStatus::Error,
           "rejects a payload that does not match the immediately projected authenticated frame");

    const ControllerErrorPayload unknown_state{
        "STATE_UNCERTAIN", ControllerErrorSeverity::Error,
        "Controller cannot determine state", {},
        std::string_view{"lid.motor"}, ControllerSubsystemStatus::Unknown};
    output = {};
    expect(projector.project(error_message(unknown_state), output) ==
               ControllerEventProjectionStatus::Projected &&
               output.payload.find("\"status\":\"UNKNOWN\"") !=
                   std::string_view::npos,
           "explicit UNKNOWN is preserved rather than treated as healthy");

    const ControllerErrorPayload recovered{
        "STATE_RESTORED", ControllerErrorSeverity::Info,
        "Subsystem state restored", {},
        std::string_view{"lid.motor"}, ControllerSubsystemStatus::Pass};
    output = {};
    expect(projector.project(error_message(recovered), output) ==
               ControllerEventProjectionStatus::Projected &&
               output.payload.find("\"status\":\"PASS\"") !=
                   std::string_view::npos,
           "a reported PASS state is preserved independently from INFO severity");

    ControllerErrorPayload partial = generic_error;
    partial.subsystem_id = std::string_view{"lid.motor"};
    output = {};
    expect(projector.project(error_message(partial), output) ==
               ControllerEventProjectionStatus::Error &&
               output.event_type.empty() && output.payload.empty(),
           "partial subsystem context fails closed");

    ControllerErrorPayload invalid_status = subsystem_error;
    invalid_status.subsystem_status = static_cast<ControllerSubsystemStatus>(255U);
    output = {};
    expect(projector.project(error_message(invalid_status), output) ==
               ControllerEventProjectionStatus::Error,
           "unknown subsystem status enum is rejected");

    const ControllerErrorPayload escaped{
        "SENSOR\"FAILED",
        ControllerErrorSeverity::Warning,
        "Line one\nLine two",
        "Quote: \\",
        std::string_view{"sensor.\"temperature"},
        ControllerSubsystemStatus::Fail};
    output = {};
    expect(projector.project(error_message(escaped), output) ==
               ControllerEventProjectionStatus::Projected &&
               output.payload.find("\\\"") != std::string_view::npos &&
               output.payload.find("\\u000a") != std::string_view::npos,
           "JSON payload escapes quotes, backslashes and control characters");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
