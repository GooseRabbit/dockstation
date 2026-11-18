#include "hardware/interfaces/controller_link_peer_session.hpp"

#include <algorithm>
#include <limits>

namespace dockstation::hardware::interfaces {
namespace {

[[nodiscard]] bool copy_identifier(
    std::array<char, kControllerLinkPeerSessionMaximumIdentifierBytes>&
        destination,
    std::size_t& destination_size,
    std::string_view source) noexcept {
    if (source.empty() ||
        source.size() > kControllerLinkPeerSessionMaximumIdentifierBytes ||
        !is_valid_controller_wire_text(source)) {
        return false;
    }
    std::copy(source.begin(), source.end(), destination.begin());
    destination_size = source.size();
    return true;
}

[[nodiscard]] std::string_view identifier_view(
    const std::array<char, kControllerLinkPeerSessionMaximumIdentifierBytes>&
        value,
    std::size_t size) noexcept {
    return {value.data(), size};
}

[[nodiscard]] bool valid_timestamp(std::string_view value) noexcept {
    return !value.empty() && is_valid_controller_wire_text(value);
}

}  // namespace

ControllerLinkPeerSession::ControllerLinkPeerSession(
    const ControllerLinkPeerSessionConfig& config) noexcept
    : protocol_version_(config.protocol_version) {
    const bool identifiers_valid =
        copy_identifier(local_controller_id_, local_controller_id_size_,
                        config.local_controller_id) &&
        copy_identifier(local_controller_instance_id_,
                        local_controller_instance_id_size_,
                        config.local_controller_instance_id) &&
        copy_identifier(peer_controller_id_, peer_controller_id_size_,
                        config.peer_controller_id);

    configured_ =
        identifiers_valid && protocol_version_.major > 0U &&
        identifier_view(local_controller_id_, local_controller_id_size_) !=
            identifier_view(peer_controller_id_, peer_controller_id_size_);
}

ControllerLinkPeerSessionResult ControllerLinkPeerSession::start() noexcept {
    if (!configured_) {
        return make_result(
            ControllerLinkPeerSessionStatus::InvalidConfiguration,
            ControllerLinkPeerSessionEvent::None);
    }
    if (state_ != ControllerLinkPeerSessionState::Stopped) {
        return make_result(ControllerLinkPeerSessionStatus::InvalidState,
                           ControllerLinkPeerSessionEvent::None);
    }

    state_ = ControllerLinkPeerSessionState::AwaitingHello;
    return make_result(ControllerLinkPeerSessionStatus::Success,
                       ControllerLinkPeerSessionEvent::None);
}

ControllerLinkPeerSessionResult ControllerLinkPeerSession::handle_message(
    const ControllerCodecDecodedMessage& message,
    std::string_view sent_at) noexcept {
    if (!configured_) {
        return make_result(
            ControllerLinkPeerSessionStatus::InvalidConfiguration,
            ControllerLinkPeerSessionEvent::None);
    }
    if (state_ == ControllerLinkPeerSessionState::Stopped) {
        return make_result(ControllerLinkPeerSessionStatus::InvalidState,
                           ControllerLinkPeerSessionEvent::None);
    }

    if (validate_controller_message(message.envelope).status !=
            ControllerMessageValidationStatus::Valid ||
        validate_controller_message_payload(message.envelope.type,
                                             message.payload).status !=
            ControllerPayloadValidationStatus::Valid) {
        return make_result(ControllerLinkPeerSessionStatus::InvalidMessage,
                           ControllerLinkPeerSessionEvent::None);
    }

    if (message.envelope.protocol_version->major != protocol_version_.major ||
        message.envelope.protocol_version->minor != protocol_version_.minor) {
        return make_result(
            ControllerLinkPeerSessionStatus::ProtocolVersionMismatch,
            ControllerLinkPeerSessionEvent::None);
    }

    if (message.envelope.controller_id !=
        identifier_view(peer_controller_id_, peer_controller_id_size_)) {
        return make_result(ControllerLinkPeerSessionStatus::PeerIdentityMismatch,
                           ControllerLinkPeerSessionEvent::None);
    }

    if (!message.envelope.sequence_number.has_value() ||
        *message.envelope.sequence_number == 0U) {
        return make_result(
            ControllerLinkPeerSessionStatus::SequenceNotIncreasing,
            ControllerLinkPeerSessionEvent::None);
    }

    if (state_ == ControllerLinkPeerSessionState::AwaitingHello) {
        if (message.envelope.type != ControllerMessageType::Hello) {
            return make_result(ControllerLinkPeerSessionStatus::UnexpectedMessage,
                               ControllerLinkPeerSessionEvent::None);
        }
        if (message.envelope.controller_instance_id.size() >
            kControllerLinkPeerSessionMaximumIdentifierBytes) {
            return make_result(ControllerLinkPeerSessionStatus::IdentifierTooLong,
                               ControllerLinkPeerSessionEvent::None);
        }
        if (!valid_timestamp(sent_at)) {
            return make_result(ControllerLinkPeerSessionStatus::InvalidTimestamp,
                               ControllerLinkPeerSessionEvent::None);
        }
        if (tx_sequence_exhausted_) {
            return make_result(ControllerLinkPeerSessionStatus::SequenceExhausted,
                               ControllerLinkPeerSessionEvent::None);
        }

        const std::string_view incoming_instance =
            message.envelope.controller_instance_id;
        const bool same_peer_instance =
            peer_controller_instance_id_size_ > 0U &&
            incoming_instance ==
                identifier_view(peer_controller_instance_id_,
                                peer_controller_instance_id_size_);
        if (same_peer_instance && peer_sequence_known_ &&
            *message.envelope.sequence_number <= last_peer_sequence_) {
            return make_result(
                ControllerLinkPeerSessionStatus::SequenceNotIncreasing,
                ControllerLinkPeerSessionEvent::None);
        }

        std::array<char, kControllerLinkPeerSessionMaximumIdentifierBytes>
            next_peer_instance{};
        std::size_t next_peer_instance_size = 0U;
        if (!copy_identifier(next_peer_instance, next_peer_instance_size,
                             incoming_instance)) {
            return make_result(ControllerLinkPeerSessionStatus::InvalidMessage,
                               ControllerLinkPeerSessionEvent::None);
        }

        const ControllerMessagePayload empty_payload{
            EmptyControllerMessagePayload{}};
        auto response = make_outbound(
            ControllerMessageType::HelloAck,
            ControllerLinkPeerSessionEvent::SessionEstablished,
            empty_payload, sent_at);
        if (response.status != ControllerLinkPeerSessionStatus::Success ||
            !response.has_outbound_message) {
            return response;
        }

        peer_controller_instance_id_ = next_peer_instance;
        peer_controller_instance_id_size_ = next_peer_instance_size;
        last_peer_sequence_ = *message.envelope.sequence_number;
        peer_sequence_known_ = true;
        state_ = ControllerLinkPeerSessionState::Established;
        response.state = state_;
        return response;
    }

    if (message.envelope.type != ControllerMessageType::Heartbeat &&
        message.envelope.type != ControllerMessageType::RequestSnapshot &&
        message.envelope.type != ControllerMessageType::CommandRequest) {
        return make_result(ControllerLinkPeerSessionStatus::UnexpectedMessage,
                           ControllerLinkPeerSessionEvent::None);
    }

    if (message.envelope.controller_instance_id !=
        identifier_view(peer_controller_instance_id_,
                        peer_controller_instance_id_size_)) {
        return make_result(ControllerLinkPeerSessionStatus::PeerInstanceMismatch,
                           ControllerLinkPeerSessionEvent::None);
    }
    if (*message.envelope.sequence_number <= last_peer_sequence_) {
        return make_result(
            ControllerLinkPeerSessionStatus::SequenceNotIncreasing,
            ControllerLinkPeerSessionEvent::None);
    }

    last_peer_sequence_ = *message.envelope.sequence_number;
    if (message.envelope.type == ControllerMessageType::CommandRequest) {
        const auto* command = std::get_if<ControllerCommandRequestPayload>(&message.payload);
        if (command == nullptr) return make_result(ControllerLinkPeerSessionStatus::InvalidMessage, ControllerLinkPeerSessionEvent::None);
        command_requested_ = command->command;
        return make_result(ControllerLinkPeerSessionStatus::Success, ControllerLinkPeerSessionEvent::CommandRequested);
    }
    if (message.envelope.type == ControllerMessageType::RequestSnapshot) {
        snapshot_requested_ = true;
        return make_result(ControllerLinkPeerSessionStatus::Success,
                           ControllerLinkPeerSessionEvent::SnapshotRequested);
    }
    return make_result(ControllerLinkPeerSessionStatus::Success,
                       ControllerLinkPeerSessionEvent::PeerHeartbeatReceived);
}


ControllerLinkPeerSessionResult ControllerLinkPeerSession::make_command_result(
    ControllerCommandType command, ControllerCommandResultStatus status,
    std::string_view reason_code, std::string_view sent_at) noexcept {
    if (!configured_ || state_ != ControllerLinkPeerSessionState::Established ||
        !command_requested_.has_value() || command_requested_.value() != command) {
        return make_result(ControllerLinkPeerSessionStatus::InvalidState,
                           ControllerLinkPeerSessionEvent::None);
    }
    if (!is_known_controller_command_result_status(status) || reason_code.empty() ||
        !is_valid_controller_wire_text(reason_code) || !valid_timestamp(sent_at)) {
        return make_result(ControllerLinkPeerSessionStatus::InvalidMessage,
                           ControllerLinkPeerSessionEvent::None);
    }
    const ControllerMessagePayload payload{
        ControllerCommandResultPayload{command, status, reason_code}};
    auto result = make_outbound(ControllerMessageType::CommandResult,
                                ControllerLinkPeerSessionEvent::None,
                                payload, sent_at);
    if (result.status == ControllerLinkPeerSessionStatus::Success &&
        result.has_outbound_message) {
        command_requested_.reset();
    }
    return result;
}

ControllerLinkPeerSessionResult
ControllerLinkPeerSession::make_telemetry_snapshot(
    const ControllerTelemetrySnapshotPayload& snapshot,
    std::string_view sent_at) noexcept {
    if (!configured_) {
        return make_result(
            ControllerLinkPeerSessionStatus::InvalidConfiguration,
            ControllerLinkPeerSessionEvent::None);
    }
    if (state_ != ControllerLinkPeerSessionState::Established ||
        !snapshot_requested_) {
        return make_result(ControllerLinkPeerSessionStatus::InvalidState,
                           ControllerLinkPeerSessionEvent::None);
    }
    if (!valid_timestamp(sent_at)) {
        return make_result(ControllerLinkPeerSessionStatus::InvalidTimestamp,
                           ControllerLinkPeerSessionEvent::None);
    }
    if (validate_controller_message_payload(
            ControllerMessageType::TelemetrySnapshot,
            ControllerMessagePayload{snapshot}).status !=
        ControllerPayloadValidationStatus::Valid) {
        return make_result(ControllerLinkPeerSessionStatus::InvalidMessage,
                           ControllerLinkPeerSessionEvent::None);
    }

    auto result = make_outbound(
        ControllerMessageType::TelemetrySnapshot,
        ControllerLinkPeerSessionEvent::None,
        ControllerMessagePayload{snapshot}, sent_at);
    if (result.status == ControllerLinkPeerSessionStatus::Success &&
        result.has_outbound_message) {
        snapshot_requested_ = false;
    }
    return result;
}

ControllerLinkPeerSessionResult ControllerLinkPeerSession::make_result(
    ControllerLinkPeerSessionStatus status,
    ControllerLinkPeerSessionEvent event) const noexcept {
    return {status, event, state_, false, {},
            ControllerMessagePayload{EmptyControllerMessagePayload{}}};
}

ControllerLinkPeerSessionResult ControllerLinkPeerSession::make_outbound(
    ControllerMessageType type,
    ControllerLinkPeerSessionEvent event,
    const ControllerMessagePayload& payload,
    std::string_view sent_at) noexcept {
    if (!valid_timestamp(sent_at)) {
        return make_result(ControllerLinkPeerSessionStatus::InvalidTimestamp,
                           ControllerLinkPeerSessionEvent::None);
    }
    if (tx_sequence_exhausted_) {
        return make_result(ControllerLinkPeerSessionStatus::SequenceExhausted,
                           ControllerLinkPeerSessionEvent::None);
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

    ControllerLinkPeerSessionResult result{};
    result.status = ControllerLinkPeerSessionStatus::Success;
    result.event = event;
    result.state = state_;
    result.has_outbound_message = true;
    result.outbound_message = envelope;
    result.outbound_payload = payload;
    return result;
}

void ControllerLinkPeerSession::stop() noexcept {
    state_ = ControllerLinkPeerSessionState::Stopped;
    snapshot_requested_ = false;
    command_requested_.reset();
}

}  // namespace dockstation::hardware::interfaces
