#include "hardware/interfaces/controller_link_session.hpp"

#include <algorithm>
#include <limits>

namespace dockstation::hardware::interfaces {
namespace {

[[nodiscard]] bool copy_identifier(
    std::array<char, kControllerLinkSessionMaximumIdentifierBytes>& destination,
    std::size_t& destination_size,
    std::string_view source) noexcept {
    if (source.empty() ||
        source.size() > kControllerLinkSessionMaximumIdentifierBytes ||
        !is_valid_controller_wire_text(source)) {
        return false;
    }
    std::copy(source.begin(), source.end(), destination.begin());
    destination_size = source.size();
    return true;
}

[[nodiscard]] std::string_view identifier_view(
    const std::array<char, kControllerLinkSessionMaximumIdentifierBytes>& value,
    std::size_t size) noexcept {
    return {value.data(), size};
}

[[nodiscard]] bool elapsed_at_least(
    std::uint64_t now_ms,
    std::uint64_t since_ms,
    std::uint64_t duration_ms) noexcept {
    // Durations are limited to less than half the uint64 range at
    // configuration time, so unsigned subtraction also handles timer wrap.
    return (now_ms - since_ms) >= duration_ms;
}

[[nodiscard]] bool is_valid_duration(std::uint64_t duration_ms) noexcept {
    return duration_ms > 0U &&
           duration_ms <= (std::numeric_limits<std::uint64_t>::max() / 2U);
}

}  // namespace

ControllerLinkSession::ControllerLinkSession(
    const ControllerLinkSessionConfig& config) noexcept
    : protocol_version_(config.protocol_version),
      handshake_timeout_ms_(config.handshake_timeout_ms),
      heartbeat_interval_ms_(config.heartbeat_interval_ms),
      heartbeat_timeout_ms_(config.heartbeat_timeout_ms),
      retry_interval_ms_(config.retry_interval_ms) {
    const bool identifiers_valid =
        copy_identifier(local_controller_id_, local_controller_id_size_,
                        config.local_controller_id) &&
        copy_identifier(local_controller_instance_id_,
                        local_controller_instance_id_size_,
                        config.local_controller_instance_id) &&
        copy_identifier(peer_controller_id_, peer_controller_id_size_,
                        config.peer_controller_id);

    const bool durations_valid =
        is_valid_duration(handshake_timeout_ms_) &&
        is_valid_duration(heartbeat_interval_ms_) &&
        is_valid_duration(heartbeat_timeout_ms_) &&
        is_valid_duration(retry_interval_ms_) &&
        heartbeat_timeout_ms_ > heartbeat_interval_ms_;

    configured_ = identifiers_valid && durations_valid &&
                  protocol_version_.major > 0U &&
                  identifier_view(local_controller_id_, local_controller_id_size_) !=
                      identifier_view(peer_controller_id_, peer_controller_id_size_);
}

ControllerLinkSessionResult ControllerLinkSession::start(
    std::uint64_t now_ms, std::string_view sent_at) noexcept {
    if (!configured_) {
        return make_result(ControllerLinkSessionStatus::InvalidConfiguration,
                           ControllerLinkSessionEvent::None);
    }
    if (state_ != ControllerLinkSessionState::Stopped &&
        state_ != ControllerLinkSessionState::LinkLost) {
        return make_result(ControllerLinkSessionStatus::InvalidState,
                           ControllerLinkSessionEvent::None);
    }
    if (sent_at.empty() || !is_valid_controller_wire_text(sent_at)) {
        return make_result(ControllerLinkSessionStatus::InvalidTimestamp,
                           ControllerLinkSessionEvent::None);
    }
    if (tx_sequence_exhausted_) {
        return make_result(ControllerLinkSessionStatus::SequenceExhausted,
                           ControllerLinkSessionEvent::None);
    }

    state_ = ControllerLinkSessionState::AwaitingHelloAck;
    last_outbound_message_ms_ = now_ms;
    return make_outbound(ControllerMessageType::Hello,
                         ControllerLinkSessionEvent::HelloRequired,
                         now_ms, sent_at);
}

ControllerLinkSessionResult ControllerLinkSession::poll(
    std::uint64_t now_ms, std::string_view sent_at) noexcept {
    if (!configured_) {
        return make_result(ControllerLinkSessionStatus::InvalidConfiguration,
                           ControllerLinkSessionEvent::None);
    }

    switch (state_) {
        case ControllerLinkSessionState::Stopped:
            return make_result(ControllerLinkSessionStatus::InvalidState,
                               ControllerLinkSessionEvent::None);

        case ControllerLinkSessionState::AwaitingHelloAck:
            if (elapsed_at_least(now_ms, last_outbound_message_ms_,
                                 handshake_timeout_ms_)) {
                state_ = ControllerLinkSessionState::LinkLost;
                last_retry_ms_ = now_ms;
                return make_result(ControllerLinkSessionStatus::HandshakeTimeout,
                                   ControllerLinkSessionEvent::LinkLost);
            }
            return make_result(ControllerLinkSessionStatus::Success,
                               ControllerLinkSessionEvent::None);

        case ControllerLinkSessionState::Established:
            if (elapsed_at_least(now_ms, last_peer_activity_ms_,
                                 heartbeat_timeout_ms_)) {
                state_ = ControllerLinkSessionState::LinkLost;
                last_retry_ms_ = now_ms;
                return make_result(ControllerLinkSessionStatus::HeartbeatTimeout,
                                   ControllerLinkSessionEvent::LinkLost);
            }
            if (elapsed_at_least(now_ms, last_outbound_message_ms_,
                                 heartbeat_interval_ms_)) {
                return make_outbound(ControllerMessageType::Heartbeat,
                                     ControllerLinkSessionEvent::HeartbeatDue,
                                     now_ms, sent_at);
            }
            return make_result(ControllerLinkSessionStatus::Success,
                               ControllerLinkSessionEvent::None);

        case ControllerLinkSessionState::LinkLost:
            if (elapsed_at_least(now_ms, last_retry_ms_, retry_interval_ms_)) {
                return start(now_ms, sent_at);
            }
            return make_result(ControllerLinkSessionStatus::Success,
                               ControllerLinkSessionEvent::None);
    }

    return make_result(ControllerLinkSessionStatus::InvalidState,
                       ControllerLinkSessionEvent::None);
}

ControllerLinkSessionResult ControllerLinkSession::handle_message(
    const ControllerCodecDecodedMessage& message,
    std::uint64_t now_ms) noexcept {
    if (!configured_) {
        return make_result(ControllerLinkSessionStatus::InvalidConfiguration,
                           ControllerLinkSessionEvent::None);
    }
    if (state_ != ControllerLinkSessionState::AwaitingHelloAck &&
        state_ != ControllerLinkSessionState::Established) {
        return make_result(ControllerLinkSessionStatus::InvalidState,
                           ControllerLinkSessionEvent::None);
    }

    if (validate_controller_message(message.envelope).status !=
            ControllerMessageValidationStatus::Valid ||
        validate_controller_message_payload(message.envelope.type,
                                             message.payload).status !=
            ControllerPayloadValidationStatus::Valid) {
        return make_result(ControllerLinkSessionStatus::InvalidMessage,
                           ControllerLinkSessionEvent::None);
    }

    if (message.envelope.protocol_version->major != protocol_version_.major ||
        message.envelope.protocol_version->minor != protocol_version_.minor) {
        return make_result(ControllerLinkSessionStatus::ProtocolVersionMismatch,
                           ControllerLinkSessionEvent::None);
    }

    if (message.envelope.controller_id !=
        identifier_view(peer_controller_id_, peer_controller_id_size_)) {
        return make_result(ControllerLinkSessionStatus::PeerIdentityMismatch,
                           ControllerLinkSessionEvent::None);
    }

    if (!message.envelope.sequence_number.has_value() ||
        *message.envelope.sequence_number == 0U) {
        return make_result(ControllerLinkSessionStatus::SequenceNotIncreasing,
                           ControllerLinkSessionEvent::None);
    }

    if (state_ == ControllerLinkSessionState::AwaitingHelloAck) {
        if (message.envelope.type != ControllerMessageType::HelloAck) {
            return make_result(ControllerLinkSessionStatus::UnexpectedMessage,
                               ControllerLinkSessionEvent::None);
        }

        const auto peer_instance = message.envelope.controller_instance_id;
        if (peer_instance.size() >
            kControllerLinkSessionMaximumIdentifierBytes) {
            return make_result(ControllerLinkSessionStatus::IdentifierTooLong,
                               ControllerLinkSessionEvent::None);
        }

        const bool same_peer_instance =
            peer_instance ==
            identifier_view(peer_controller_instance_id_,
                            peer_controller_instance_id_size_);
        if (same_peer_instance && peer_sequence_known_ &&
            *message.envelope.sequence_number <= last_peer_sequence_) {
            return make_result(
                ControllerLinkSessionStatus::SequenceNotIncreasing,
                ControllerLinkSessionEvent::None);
        }

        if (!copy_identifier(peer_controller_instance_id_,
                             peer_controller_instance_id_size_,
                             peer_instance)) {
            return make_result(ControllerLinkSessionStatus::InvalidMessage,
                               ControllerLinkSessionEvent::None);
        }

        last_peer_sequence_ = *message.envelope.sequence_number;
        peer_sequence_known_ = true;
        last_peer_activity_ms_ = now_ms;
        last_outbound_message_ms_ = now_ms;
        state_ = ControllerLinkSessionState::Established;
        return make_result(ControllerLinkSessionStatus::Success,
                           ControllerLinkSessionEvent::SessionEstablished);
    }

    if (message.envelope.type != ControllerMessageType::Heartbeat &&
        message.envelope.type != ControllerMessageType::TelemetrySnapshot &&
        message.envelope.type != ControllerMessageType::Error) {
        return make_result(ControllerLinkSessionStatus::UnexpectedMessage,
                           ControllerLinkSessionEvent::None);
    }

    if (message.envelope.controller_instance_id !=
        identifier_view(peer_controller_instance_id_,
                        peer_controller_instance_id_size_)) {
        return make_result(ControllerLinkSessionStatus::PeerInstanceMismatch,
                           ControllerLinkSessionEvent::None);
    }
    if (*message.envelope.sequence_number <= last_peer_sequence_) {
        return make_result(ControllerLinkSessionStatus::SequenceNotIncreasing,
                           ControllerLinkSessionEvent::None);
    }

    last_peer_sequence_ = *message.envelope.sequence_number;
    last_peer_activity_ms_ = now_ms;
    return make_result(
        ControllerLinkSessionStatus::Success,
        message.envelope.type == ControllerMessageType::Heartbeat
            ? ControllerLinkSessionEvent::PeerHeartbeatReceived
            : ControllerLinkSessionEvent::PeerMessageReceived);
}

ControllerLinkSessionResult ControllerLinkSession::make_result(
    ControllerLinkSessionStatus status,
    ControllerLinkSessionEvent event) const noexcept {
    return {status, event, state_, false, {}};
}

ControllerLinkSessionResult ControllerLinkSession::make_outbound(
    ControllerMessageType type,
    ControllerLinkSessionEvent event,
    std::uint64_t now_ms,
    std::string_view sent_at) noexcept {
    if (sent_at.empty() || !is_valid_controller_wire_text(sent_at)) {
        return make_result(ControllerLinkSessionStatus::InvalidTimestamp,
                           ControllerLinkSessionEvent::None);
    }
    if (tx_sequence_exhausted_) {
        return make_result(ControllerLinkSessionStatus::SequenceExhausted,
                           ControllerLinkSessionEvent::None);
    }

    ControllerMessageEnvelope envelope{};
    envelope.type = type;
    envelope.protocol_version = protocol_version_;
    envelope.controller_id =
        identifier_view(local_controller_id_, local_controller_id_size_);
    envelope.controller_instance_id =
        identifier_view(local_controller_instance_id_,
                        local_controller_instance_id_size_);
    envelope.sequence_number = next_tx_sequence_;
    envelope.sent_at = sent_at;

    if (next_tx_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        tx_sequence_exhausted_ = true;
    } else {
        ++next_tx_sequence_;
    }
    last_outbound_message_ms_ = now_ms;

    ControllerLinkSessionResult result{};
    result.status = ControllerLinkSessionStatus::Success;
    result.event = event;
    result.state = state_;
    result.has_outbound_message = true;
    result.outbound_message = envelope;
    return result;
}

void ControllerLinkSession::stop() noexcept {
    state_ = ControllerLinkSessionState::Stopped;
    last_outbound_message_ms_ = 0U;
    last_peer_activity_ms_ = 0U;
    last_retry_ms_ = 0U;
}

}  // namespace dockstation::hardware::interfaces
