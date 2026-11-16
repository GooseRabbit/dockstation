#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <variant>

#include "hardware/interfaces/controller_link_peer_session.hpp"

using namespace dockstation::hardware::interfaces;

static_assert(!std::is_copy_constructible_v<ControllerLinkPeerSession>);
static_assert(!std::is_copy_assignable_v<ControllerLinkPeerSession>);
static_assert(!std::is_move_constructible_v<ControllerLinkPeerSession>);
static_assert(!std::is_move_assignable_v<ControllerLinkPeerSession>);

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

ControllerLinkPeerSessionConfig valid_config() {
    ControllerLinkPeerSessionConfig config{};
    config.local_controller_id = "esp32-01";
    config.local_controller_instance_id = "esp32-boot-01";
    config.peer_controller_id = "repka-01";
    config.protocol_version = {1U, 1U};
    return config;
}

ControllerCodecDecodedMessage make_message(
    ControllerMessageType type,
    std::string_view controller_id = "repka-01",
    std::string_view instance_id = "repka-boot-01",
    std::uint64_t sequence = 1U,
    ControllerProtocolVersion version = {1U, 1U},
    ControllerMessagePayload payload =
        ControllerMessagePayload{EmptyControllerMessagePayload{}}) {
    ControllerCodecDecodedMessage message{};
    message.envelope.type = type;
    message.envelope.protocol_version = version;
    message.envelope.controller_id = controller_id;
    message.envelope.controller_instance_id = instance_id;
    message.envelope.sequence_number = sequence;
    message.envelope.sent_at = "1700000000000";
    message.payload = payload;
    return message;
}

bool round_trip(
    const ControllerMessageEnvelope& envelope,
    const ControllerMessagePayload& payload,
    ControllerCodecDecodedMessage& decoded) {
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize> frame{};
    const auto encoded = encode_controller_message(
        envelope, payload, frame.data(), frame.size());
    if (encoded.status != ControllerCodecStatus::Success) {
        return false;
    }
    ControllerCodecDecodeStorage storage{};
    const auto result = decode_controller_message(
        frame.data(), encoded.frame_size, storage, decoded);
    return result.status == ControllerCodecStatus::Success;
}
}  // namespace

int main() {
    ControllerLinkPeerSession invalid_session(
        ControllerLinkPeerSessionConfig{});
    expect(!invalid_session.is_configured() &&
               invalid_session.start().status ==
                   ControllerLinkPeerSessionStatus::InvalidConfiguration,
           "rejects an incomplete peer-session configuration");

    auto config = valid_config();
    ControllerLinkPeerSession session(config);
    expect(session.is_configured() &&
               session.state() == ControllerLinkPeerSessionState::Stopped,
           "accepts a complete bounded configuration and starts Stopped");

    auto result = session.handle_message(
        make_message(ControllerMessageType::Hello), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::InvalidState,
           "does not accept messages before authenticated transport start");

    result = session.start();
    expect(result.status == ControllerLinkPeerSessionStatus::Success &&
               session.state() == ControllerLinkPeerSessionState::AwaitingHello,
           "starts protocol processing after the caller authenticates transport");

    result = session.start();
    expect(result.status == ControllerLinkPeerSessionStatus::InvalidState,
           "does not restart an active protocol handshake");

    result = session.handle_message(
        make_message(ControllerMessageType::RequestSnapshot), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::UnexpectedMessage &&
               session.state() == ControllerLinkPeerSessionState::AwaitingHello,
           "rejects snapshot requests before HELLO");

    result = session.handle_message(
        make_message(ControllerMessageType::Hello, "other-repka"), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::PeerIdentityMismatch,
           "rejects HELLO from an unexpected controller ID");

    result = session.handle_message(
        make_message(ControllerMessageType::Hello, "repka-01",
                     "repka-boot-01", 1U, {1U, 0U}), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::ProtocolVersionMismatch,
           "rejects a protocol downgrade or mismatched version");

    result = session.handle_message(
        make_message(ControllerMessageType::Hello, "repka-01",
                     "repka-boot-01", 1U, {1U, 1U},
                     ControllerMessagePayload{ControllerErrorPayload{
                         "BAD", ControllerErrorSeverity::Error, "unexpected", {}}}),
        "time");
    expect(result.status == ControllerLinkPeerSessionStatus::InvalidMessage,
           "rejects an invalid HELLO payload");

    result = session.handle_message(
        make_message(ControllerMessageType::Hello), "");
    expect(result.status == ControllerLinkPeerSessionStatus::InvalidTimestamp &&
               session.state() == ControllerLinkPeerSessionState::AwaitingHello,
           "does not establish a session when the HELLO_ACK timestamp is invalid");

    result = session.handle_message(
        make_message(ControllerMessageType::Hello), "1700000000000");
    expect(result.status == ControllerLinkPeerSessionStatus::Success &&
               result.event == ControllerLinkPeerSessionEvent::SessionEstablished &&
               result.state == ControllerLinkPeerSessionState::Established &&
               result.has_outbound_message &&
               result.outbound_message.type == ControllerMessageType::HelloAck &&
               result.outbound_message.controller_id == "esp32-01" &&
               result.outbound_message.controller_instance_id == "esp32-boot-01" &&
               result.outbound_message.sequence_number == 1U,
           "accepts HELLO and returns a sequenced HELLO_ACK");

    ControllerCodecDecodedMessage decoded{};
    expect(round_trip(result.outbound_message, result.outbound_payload, decoded) &&
               decoded.envelope.type == ControllerMessageType::HelloAck &&
               decoded.envelope.controller_id == "esp32-01" &&
               decoded.envelope.sequence_number == 1U,
           "HELLO_ACK round-trips through the production ControllerCodec");

    result = session.handle_message(
        make_message(ControllerMessageType::RequestSnapshot,
                     "repka-01", "other-boot", 2U), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::PeerInstanceMismatch,
           "rejects established-session traffic from another peer instance");

    result = session.handle_message(
        make_message(ControllerMessageType::Heartbeat,
                     "repka-01", "repka-boot-01", 1U), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::SequenceNotIncreasing,
           "rejects a duplicate peer sequence");

    result = session.handle_message(
        make_message(ControllerMessageType::RequestSnapshot,
                     "repka-01", "repka-boot-01", 2U), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::Success &&
               result.event == ControllerLinkPeerSessionEvent::SnapshotRequested &&
               !result.has_outbound_message,
           "accepts REQUEST_SNAPSHOT as an application event without faking sensor data");

    result = session.handle_message(
        make_message(ControllerMessageType::Heartbeat,
                     "repka-01", "repka-boot-01", 3U), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::Success &&
               result.event == ControllerLinkPeerSessionEvent::PeerHeartbeatReceived,
           "accepts an increasing HEARTBEAT from the established peer");

    result = session.handle_message(
        make_message(ControllerMessageType::TelemetrySnapshot,
                     "repka-01", "repka-boot-01", 4U), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::UnexpectedMessage,
           "rejects controller-to-peer telemetry in the request direction");

    result = session.handle_message(
        make_message(ControllerMessageType::RequestSnapshot,
                     "repka-01", "repka-boot-01", 4U), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::Success &&
               result.event == ControllerLinkPeerSessionEvent::SnapshotRequested,
           "does not consume a sequence number for a rejected message");

    const ControllerTelemetryMeasurement invalid_measurement{
        "", 23.5, "degC", SensorQualityStatus::Ok, "1700000000000"};
    result = session.make_telemetry_snapshot(
        ControllerTelemetrySnapshotPayload{&invalid_measurement, 1U},
        "1700000000000");
    expect(result.status == ControllerLinkPeerSessionStatus::InvalidMessage &&
               !result.has_outbound_message,
           "rejects invalid telemetry before consuming an outbound sequence");

    const std::array<ControllerTelemetryMeasurement, 2U> measurements{{
        {"climate.temperature", 23.5, "degC", SensorQualityStatus::Ok,
         "1700000000000"},
        {"lid.closed", true, "bool", SensorQualityStatus::NotConfigured,
         "1700000000000"}
    }};
    result = session.make_telemetry_snapshot(
        ControllerTelemetrySnapshotPayload{measurements.data(),
                                            measurements.size()},
        "1700000000200");
    expect(result.status == ControllerLinkPeerSessionStatus::Success &&
               result.has_outbound_message &&
               result.outbound_message.type ==
                   ControllerMessageType::TelemetrySnapshot &&
               result.outbound_message.sequence_number == 2U,
           "builds a typed telemetry response using the shared outbound sequence");

    expect(round_trip(result.outbound_message, result.outbound_payload, decoded) &&
               decoded.envelope.type == ControllerMessageType::TelemetrySnapshot &&
               decoded.envelope.sequence_number == 2U &&
               std::get<ControllerTelemetrySnapshotPayload>(
                   decoded.payload).measurement_count == 2U,
           "telemetry response round-trips with both typed measurements");

    session.stop();
    expect(session.state() == ControllerLinkPeerSessionState::Stopped &&
               session.make_telemetry_snapshot(
                   ControllerTelemetrySnapshotPayload{}, "time").status ==
                   ControllerLinkPeerSessionStatus::InvalidState,
           "stop closes the session and prevents further telemetry output");

    expect(session.start().status == ControllerLinkPeerSessionStatus::Success,
           "restarts protocol processing without recreating the session object");
    result = session.handle_message(
        make_message(ControllerMessageType::Hello,
                     "repka-01", "repka-boot-01", 4U), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::SequenceNotIncreasing,
           "retains replay protection for the same peer instance after reconnect");

    result = session.handle_message(
        make_message(ControllerMessageType::Hello,
                     "repka-01", "repka-boot-01", 5U), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::Success &&
               result.outbound_message.sequence_number == 3U,
           "continues outbound sequence numbers after same-instance reconnect");

    session.stop();
    expect(session.start().status == ControllerLinkPeerSessionStatus::Success,
           "starts a subsequent peer instance handshake");
    result = session.handle_message(
        make_message(ControllerMessageType::Hello,
                     "repka-01", "repka-boot-02", 1U), "time");
    expect(result.status == ControllerLinkPeerSessionStatus::Success &&
               result.outbound_message.type == ControllerMessageType::HelloAck &&
               result.outbound_message.sequence_number == 4U,
           "accepts a new peer instance while preserving local transmit sequence");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
