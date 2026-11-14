#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

#include "hardware/interfaces/controller_link_session.hpp"

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

ControllerLinkSessionConfig valid_config() {
    ControllerLinkSessionConfig config{};
    config.local_controller_id = "repka-01";
    config.local_controller_instance_id = "repka-boot-01";
    config.peer_controller_id = "esp32-01";
    config.protocol_version = {1U, 0U};
    config.handshake_timeout_ms = 3000U;
    config.heartbeat_interval_ms = 1000U;
    config.heartbeat_timeout_ms = 5000U;
    config.retry_interval_ms = 1000U;
    return config;
}

ControllerCodecDecodedMessage make_message(
    ControllerMessageType type,
    std::string_view controller_id = "esp32-01",
    std::string_view instance_id = "esp32-boot-01",
    std::uint64_t sequence = 1U,
    ControllerProtocolVersion version = {1U, 0U},
    ControllerMessagePayload payload =
        ControllerMessagePayload{EmptyControllerMessagePayload{}}) {
    ControllerCodecDecodedMessage message{};
    message.envelope.type = type;
    message.envelope.protocol_version = version;
    message.envelope.controller_id = controller_id;
    message.envelope.controller_instance_id = instance_id;
    message.envelope.sequence_number = sequence;
    message.envelope.sent_at = "2026-11-14T19:00:00Z";
    message.payload = payload;
    return message;
}
}  // namespace

int main() {
    const auto config = valid_config();
    ControllerLinkSession session(config);
    expect(session.is_configured(), "accepts a complete bounded configuration");
    expect(session.state() == ControllerLinkSessionState::Stopped,
           "starts in Stopped state");

    auto result = session.poll(0U, "time");
    expect(result.status == ControllerLinkSessionStatus::InvalidState,
           "does not poll before start");

    result = session.start(100U, "2026-11-14T19:00:00Z");
    expect(result.status == ControllerLinkSessionStatus::Success &&
               result.event == ControllerLinkSessionEvent::HelloRequired &&
               result.state == ControllerLinkSessionState::AwaitingHelloAck &&
               result.has_outbound_message,
           "start emits HELLO and enters AwaitingHelloAck");
    expect(result.outbound_message.type == ControllerMessageType::Hello &&
               result.outbound_message.controller_id == "repka-01" &&
               result.outbound_message.controller_instance_id == "repka-boot-01" &&
               result.outbound_message.sequence_number == 1U &&
               result.outbound_message.protocol_version->major == 1U &&
               result.outbound_message.protocol_version->minor == 0U,
           "HELLO contains configured local identity, version and first sequence");
    expect(validate_controller_message(result.outbound_message).status ==
               ControllerMessageValidationStatus::Valid &&
               validate_controller_message_payload(
                   result.outbound_message.type,
                   ControllerMessagePayload{EmptyControllerMessagePayload{}}).status ==
               ControllerPayloadValidationStatus::Valid,
           "outbound HELLO satisfies the shared protocol validators");

    std::array<std::uint8_t, kControllerCodecMaximumFrameSize> outbound_frame{};
    const auto outbound_encode = encode_controller_message(
        result.outbound_message,
        ControllerMessagePayload{EmptyControllerMessagePayload{}},
        outbound_frame.data(), outbound_frame.size());
    ControllerCodecDecodeStorage outbound_storage{};
    ControllerCodecDecodedMessage outbound_decoded{};
    const auto outbound_decode = decode_controller_message(
        outbound_frame.data(), outbound_encode.frame_size,
        outbound_storage, outbound_decoded);
    expect(outbound_encode.status == ControllerCodecStatus::Success &&
               outbound_decode.status == ControllerCodecStatus::Success &&
               outbound_decoded.envelope.type == ControllerMessageType::Hello,
           "session HELLO envelope round-trips through the production codec");

    result = session.start(101U, "time");
    expect(result.status == ControllerLinkSessionStatus::InvalidState,
           "does not restart an active handshake implicitly");

    result = session.handle_message(
        make_message(ControllerMessageType::Heartbeat), 200U);
    expect(result.status == ControllerLinkSessionStatus::UnexpectedMessage &&
               session.state() == ControllerLinkSessionState::AwaitingHelloAck,
           "does not accept HEARTBEAT before HELLO_ACK");

    result = session.handle_message(
        make_message(ControllerMessageType::HelloAck, "other-esp"), 200U);
    expect(result.status == ControllerLinkSessionStatus::PeerIdentityMismatch &&
               session.state() == ControllerLinkSessionState::AwaitingHelloAck,
           "rejects HELLO_ACK from an unexpected peer ID");

    result = session.handle_message(
        make_message(ControllerMessageType::HelloAck, "esp32-01",
                     "esp32-boot-01", 1U, {2U, 0U}), 200U);
    expect(result.status == ControllerLinkSessionStatus::ProtocolVersionMismatch,
           "rejects a different logical protocol version");

    result = session.handle_message(
        make_message(ControllerMessageType::HelloAck, "esp32-01",
                     "esp32-boot-01", 1U, {1U, 0U},
                     ControllerMessagePayload{ControllerErrorPayload{
                         "BAD", ControllerErrorSeverity::Error, "unexpected", {}}}),
        200U);
    expect(result.status == ControllerLinkSessionStatus::InvalidMessage,
           "rejects a payload type that does not match HELLO_ACK");

    auto hello_ack = make_message(ControllerMessageType::HelloAck);
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize> inbound_frame{};
    const auto inbound_encode = encode_controller_message(
        hello_ack.envelope, hello_ack.payload,
        inbound_frame.data(), inbound_frame.size());
    ControllerCodecDecodeStorage inbound_storage{};
    ControllerCodecDecodedMessage inbound_decoded{};
    const auto inbound_decode = decode_controller_message(
        inbound_frame.data(), inbound_encode.frame_size,
        inbound_storage, inbound_decoded);
    result = session.handle_message(inbound_decoded, 200U);
    expect(inbound_encode.status == ControllerCodecStatus::Success &&
               inbound_decode.status == ControllerCodecStatus::Success &&
               result.status == ControllerLinkSessionStatus::Success &&
               result.event == ControllerLinkSessionEvent::SessionEstablished &&
               session.state() == ControllerLinkSessionState::Established,
           "establishes the protocol session after a codec-decoded HELLO_ACK");

    result = session.handle_message(
        make_message(ControllerMessageType::Heartbeat, "esp32-01",
                     "esp32-boot-01", 1U), 300U);
    expect(result.status == ControllerLinkSessionStatus::SequenceNotIncreasing,
           "rejects a duplicate or non-increasing peer sequence");

    result = session.handle_message(
        make_message(ControllerMessageType::Heartbeat, "esp32-01",
                     "different-boot", 2U), 300U);
    expect(result.status == ControllerLinkSessionStatus::PeerInstanceMismatch,
           "rejects frames from a different peer boot instance");

    result = session.handle_message(
        make_message(ControllerMessageType::Heartbeat, "esp32-01",
                     "esp32-boot-01", 2U), 500U);
    expect(result.status == ControllerLinkSessionStatus::Success &&
               result.event == ControllerLinkSessionEvent::PeerHeartbeatReceived,
           "accepts an increasing heartbeat from the established peer");

    result = session.poll(1199U, "time");
    expect(!result.has_outbound_message &&
               result.event == ControllerLinkSessionEvent::None,
           "does not emit heartbeat before the configured interval");
    result = session.poll(1200U, "2026-11-14T19:00:01Z");
    expect(result.status == ControllerLinkSessionStatus::Success &&
               result.event == ControllerLinkSessionEvent::HeartbeatDue &&
               result.has_outbound_message &&
               result.outbound_message.type == ControllerMessageType::Heartbeat &&
               result.outbound_message.sequence_number == 2U,
           "emits the next monotonic local HEARTBEAT when due");

    // An accepted telemetry packet is valid peer activity and advances the
    // shared inbound sequence, but this session does not interpret the data.
    const ControllerTelemetryMeasurement measurement{
        "lid.closed", true, "bool", SensorQualityStatus::Ok, "time"};
    result = session.handle_message(
        make_message(ControllerMessageType::TelemetrySnapshot, "esp32-01",
                     "esp32-boot-01", 3U, {1U, 0U},
                     ControllerMessagePayload{ControllerTelemetrySnapshotPayload{
                         &measurement, 1U}}), 1600U);
    expect(result.status == ControllerLinkSessionStatus::Success &&
               result.event == ControllerLinkSessionEvent::PeerMessageReceived,
           "accepts a valid sequenced telemetry message without processing its data");

    const ControllerErrorPayload peer_error{
        "SENSOR_WARN", ControllerErrorSeverity::Warning, "sensor warning", {}};
    result = session.handle_message(
        make_message(ControllerMessageType::Error, "esp32-01",
                     "esp32-boot-01", 4U, {1U, 0U},
                     ControllerMessagePayload{peer_error}), 1700U);
    expect(result.status == ControllerLinkSessionStatus::Success &&
               result.event == ControllerLinkSessionEvent::PeerMessageReceived,
           "accepts a valid sequenced ERROR message for upper-layer handling");

    result = session.poll(6699U, "time");
    expect(result.state == ControllerLinkSessionState::Established &&
               result.has_outbound_message &&
               result.event == ControllerLinkSessionEvent::HeartbeatDue,
           "keeps session established and sends a due heartbeat before peer timeout");
    result = session.poll(6700U, "time");
    expect(result.status == ControllerLinkSessionStatus::HeartbeatTimeout &&
               result.event == ControllerLinkSessionEvent::LinkLost &&
               session.state() == ControllerLinkSessionState::LinkLost,
           "reports link loss after the configured peer activity timeout");

    result = session.poll(7699U, "time");
    expect(result.state == ControllerLinkSessionState::LinkLost &&
               !result.has_outbound_message,
           "waits for the retry interval after link loss");
    result = session.poll(7700U, "2026-11-14T19:00:07Z");
    expect(result.status == ControllerLinkSessionStatus::Success &&
               result.event == ControllerLinkSessionEvent::HelloRequired &&
               result.outbound_message.type == ControllerMessageType::Hello &&
               result.outbound_message.sequence_number == 4U &&
               session.state() == ControllerLinkSessionState::AwaitingHelloAck,
           "automatically retries HELLO with a higher local sequence");

    result = session.handle_message(
        make_message(ControllerMessageType::HelloAck, "esp32-01",
                     "esp32-boot-01", 5U), 7800U);
    expect(result.status == ControllerLinkSessionStatus::Success,
           "accepts a higher HELLO_ACK sequence from the same peer instance");

    result = session.poll(12799U, "2026-11-14T19:00:12Z");
    expect(result.status == ControllerLinkSessionStatus::Success &&
               result.event == ControllerLinkSessionEvent::HeartbeatDue &&
               result.outbound_message.sequence_number == 5U,
           "sends the next heartbeat while the peer activity timeout has not elapsed");

    result = session.poll(12800U, "time");
    expect(result.status == ControllerLinkSessionStatus::HeartbeatTimeout &&
               session.state() == ControllerLinkSessionState::LinkLost,
           "marks an established session lost when peer activity times out");

    result = session.poll(13799U, "time");
    expect(result.state == ControllerLinkSessionState::LinkLost &&
               !result.has_outbound_message,
           "waits for the retry interval after the second link loss");

    result = session.poll(13800U, "2026-11-14T19:00:13Z");
    expect(result.status == ControllerLinkSessionStatus::Success &&
               result.outbound_message.sequence_number == 6U &&
               result.outbound_message.type == ControllerMessageType::Hello,
           "restarts HELLO with a monotonic sequence after peer timeout");

    result = session.handle_message(
        make_message(ControllerMessageType::HelloAck, "esp32-01",
                     "esp32-boot-02", 1U), 13900U);
    expect(result.status == ControllerLinkSessionStatus::Success &&
               result.event == ControllerLinkSessionEvent::SessionEstablished,
           "accepts a new peer instance after its sequence restarts on a fresh handshake");

    session.stop();
    expect(session.state() == ControllerLinkSessionState::Stopped,
           "stop returns the state machine to Stopped");
    result = session.start(14000U, "time");
    expect(result.status == ControllerLinkSessionStatus::Success &&
               result.outbound_message.sequence_number == 7U,
           "stop/start does not reset the local sequence counter");

    session.stop();
    expect(session.handle_message(
               make_message(ControllerMessageType::HelloAck), 13000U).status ==
               ControllerLinkSessionStatus::InvalidState,
           "does not accept messages while stopped");

    ControllerLinkSessionConfig invalid_config = valid_config();
    invalid_config.heartbeat_timeout_ms = invalid_config.heartbeat_interval_ms;
    ControllerLinkSession invalid_session(invalid_config);
    expect(!invalid_session.is_configured() &&
               invalid_session.start(0U, "time").status ==
                   ControllerLinkSessionStatus::InvalidConfiguration,
           "rejects invalid heartbeat timeout configuration");

    invalid_config = valid_config();
    invalid_config.peer_controller_id = "repka-01";
    ControllerLinkSession same_identity_session(invalid_config);
    expect(!same_identity_session.is_configured(),
           "rejects identical local and peer controller IDs");

    invalid_config = valid_config();
    invalid_config.local_controller_id = "";
    ControllerLinkSession missing_id_session(invalid_config);
    expect(!missing_id_session.is_configured(),
           "rejects an empty local controller ID");

    const char invalid_utf8_id[] = {
        static_cast<char>(0xC0), static_cast<char>(0xAF)};
    invalid_config = valid_config();
    invalid_config.peer_controller_id =
        std::string_view{invalid_utf8_id, sizeof(invalid_utf8_id)};
    ControllerLinkSession invalid_utf8_session(invalid_config);
    expect(!invalid_utf8_session.is_configured(),
           "rejects a peer ID that cannot be encoded as controller wire text");

    ControllerLinkSession timestamp_session(valid_config());
    expect(timestamp_session.start(0U, "").status ==
               ControllerLinkSessionStatus::InvalidTimestamp,
           "rejects an empty outbound timestamp without starting");

    ControllerLinkSession no_timestamp_session(valid_config());
    static_cast<void>(no_timestamp_session.start(0U, "time"));
    static_cast<void>(no_timestamp_session.handle_message(
        make_message(ControllerMessageType::HelloAck), 10U));
    expect(no_timestamp_session.poll(1010U, "").status ==
               ControllerLinkSessionStatus::InvalidTimestamp,
           "does not emit heartbeat without a timestamp");

    ControllerLinkSession zero_sequence_session(valid_config());
    static_cast<void>(zero_sequence_session.start(0U, "time"));
    expect(zero_sequence_session.handle_message(
               make_message(ControllerMessageType::HelloAck, "esp32-01",
                            "esp32-boot-01", 0U), 1U).status ==
               ControllerLinkSessionStatus::SequenceNotIncreasing,
           "rejects zero peer sequence on handshake");

    ControllerLinkSession wrap_session(valid_config());
    const auto near_wrap = std::numeric_limits<std::uint64_t>::max() - 100U;
    static_cast<void>(wrap_session.start(near_wrap, "time"));
    expect(wrap_session.handle_message(
               make_message(ControllerMessageType::HelloAck), near_wrap + 50U).status ==
               ControllerLinkSessionStatus::Success,
           "establishes a session near the monotonic clock counter boundary");
    expect(!wrap_session.poll(948U, "time").has_outbound_message,
           "does not send heartbeat one millisecond before a wrapped deadline");
    expect(wrap_session.poll(949U, "time").event ==
               ControllerLinkSessionEvent::HeartbeatDue,
           "sends heartbeat at the exact deadline after monotonic counter wrap");

    std::string_view too_long_identifier(
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    invalid_config = valid_config();
    invalid_config.peer_controller_id = too_long_identifier;
    ControllerLinkSession too_long_config_session(invalid_config);
    expect(!too_long_config_session.is_configured(),
           "rejects configured identifiers longer than the fixed bound");

    std::array<char, kControllerLinkSessionMaximumIdentifierBytes + 1U>
        too_long_peer_instance{};
    too_long_peer_instance.fill('x');
    ControllerLinkSession instance_length_session(valid_config());
    static_cast<void>(instance_length_session.start(0U, "time"));
    const auto long_instance_message = make_message(
        ControllerMessageType::HelloAck,
        "esp32-01",
        std::string_view{too_long_peer_instance.data(), too_long_peer_instance.size()},
        1U);
    expect(instance_length_session.handle_message(long_instance_message, 1U).status ==
               ControllerLinkSessionStatus::IdentifierTooLong,
           "rejects oversized peer instance identifiers");

    ControllerLinkSession stopped_retry_session(valid_config());
    static_cast<void>(stopped_retry_session.start(0U, "time"));
    expect(stopped_retry_session.poll(3000U, "time").status ==
               ControllerLinkSessionStatus::HandshakeTimeout,
           "handshake timeout enters LinkLost");
    expect(stopped_retry_session.poll(4000U, "").status ==
               ControllerLinkSessionStatus::InvalidTimestamp &&
               stopped_retry_session.state() == ControllerLinkSessionState::LinkLost,
           "failed retry timestamp leaves the session in LinkLost");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
