#include "controller_link_peer_service.hpp"

#include <mbedtls/platform_util.h>

namespace dockstation::firmware::esp32 {
namespace {

using hardware::interfaces::ControllerCodecStatus;
using hardware::interfaces::ControllerLinkPeerSessionEvent;
using hardware::interfaces::ControllerLinkPeerSessionStatus;

constexpr std::size_t kMaximumInboundReadsPerPoll = 4U;
constexpr std::size_t kMaximumFramesPerPoll = 4U;

[[nodiscard]] bool map_command(
    hardware::interfaces::ControllerCommandType command,
    ControllerStationCommandType& mapped) noexcept {
    switch (command) {
        case hardware::interfaces::ControllerCommandType::OpenLid:
            mapped = ControllerStationCommandType::OpenLid; return true;
        case hardware::interfaces::ControllerCommandType::CloseLid:
            mapped = ControllerStationCommandType::CloseLid; return true;
        case hardware::interfaces::ControllerCommandType::StartCleaning:
            mapped = ControllerStationCommandType::StartCleaning; return true;
        case hardware::interfaces::ControllerCommandType::StopCleaning:
            mapped = ControllerStationCommandType::StopCleaning; return true;
        case hardware::interfaces::ControllerCommandType::RotatePlatform:
            mapped = ControllerStationCommandType::RotatePlatform; return true;
        case hardware::interfaces::ControllerCommandType::StopMotion:
            mapped = ControllerStationCommandType::StopMotion; return true;
    }
    return false;
}


}  // namespace

ControllerLinkPeerService::~ControllerLinkPeerService() {
    stop();
}

bool ControllerLinkPeerService::start(
    const ControllerMbedTlsPskCredentials& credentials,
    const hardware::interfaces::ControllerLinkPeerSessionConfig& session_config,
    void* transport_context,
    mbedtls_ssl_send_t* send_callback,
    mbedtls_ssl_recv_t* receive_callback) noexcept {
    if (session_.has_value() ||
        (state_ != ControllerLinkPeerServiceState::NotConfigured &&
         state_ != ControllerLinkPeerServiceState::Stopped)) {
        return false;
    }

    session_.emplace(session_config);
    if (!session_->is_configured()) {
        last_protocol_status_ =
            ControllerLinkPeerSessionStatus::InvalidConfiguration;
        state_ = ControllerLinkPeerServiceState::Error;
        return false;
    }

    if (!tls_server_.initialize(
            credentials, transport_context, send_callback, receive_callback)) {
        last_mbedtls_error_ = tls_server_.last_mbedtls_error();
        state_ = ControllerLinkPeerServiceState::Error;
        return false;
    }

    begin_tls_handshake();
    return true;
}

bool ControllerLinkPeerService::restart_transport(
    const ControllerMbedTlsPskCredentials& credentials,
    void* transport_context,
    mbedtls_ssl_send_t* send_callback,
    mbedtls_ssl_recv_t* receive_callback) noexcept {
    if (!session_.has_value() || !session_->is_configured() ||
        (state_ != ControllerLinkPeerServiceState::Stopped &&
         state_ != ControllerLinkPeerServiceState::Error)) {
        return false;
    }

    session_->stop();
    if (!tls_server_.initialize(
            credentials, transport_context, send_callback, receive_callback)) {
        last_mbedtls_error_ = tls_server_.last_mbedtls_error();
        state_ = ControllerLinkPeerServiceState::Error;
        return false;
    }

    begin_tls_handshake();
    return true;
}

void ControllerLinkPeerService::poll(std::string_view sent_at) noexcept {
    if (state_ == ControllerLinkPeerServiceState::TlsHandshake) {
        const auto progress = tls_server_.advance_handshake();
        if (progress == ControllerMbedTlsHandshakeProgress::NeedRead ||
            progress == ControllerMbedTlsHandshakeProgress::NeedWrite) {
            return;
        }
        if (progress != ControllerMbedTlsHandshakeProgress::Complete) {
            fail();
            return;
        }

        const auto started = session_->start();
        last_protocol_status_ = started.status;
        if (started.status != ControllerLinkPeerSessionStatus::Success) {
            fail();
            return;
        }
        state_ = ControllerLinkPeerServiceState::AwaitingHello;
        return;
    }

    if (state_ != ControllerLinkPeerServiceState::AwaitingHello &&
        state_ != ControllerLinkPeerServiceState::Established) {
        return;
    }

    if (outbound_frame_size_ != 0U &&
        !flush_outbound_message()) {
        return;
    }

    if (!process_pending_plaintext(sent_at)) {
        return;
    }

    if (outbound_frame_size_ != 0U &&
        !flush_outbound_message()) {
        return;
    }

    if (inbound_size_ != 0U) {
        return;
    }

    for (std::size_t attempt = 0U;
         attempt < kMaximumInboundReadsPerPoll &&
         inbound_size_ == 0U;
         ++attempt) {
        const auto received = tls_server_.read_application_data(
            inbound_buffer_.data(), inbound_buffer_.size());

        if (received.status == ControllerMbedTlsIoStatus::WouldBlock) {
            break;
        }
        if (received.status == ControllerMbedTlsIoStatus::Closed) {
            stop();
            return;
        }
        if (received.status != ControllerMbedTlsIoStatus::Success) {
            fail();
            return;
        }
        if (received.bytes_transferred == 0U) {
            break;
        }

        inbound_size_ = received.bytes_transferred;
        inbound_offset_ = 0U;
        if (!process_pending_plaintext(sent_at)) {
            return;
        }
        if (outbound_frame_size_ != 0U) {
            static_cast<void>(flush_outbound_message());
            return;
        }
    }
}


bool ControllerLinkPeerService::handle_command_request(
    std::string_view sent_at) noexcept {
    if (!logical_session_established_ || outbound_frame_size_ != 0U ||
        !session_.has_value()) {
        return false;
    }

    const auto pending = session_->pending_command();
    if (!pending.has_value()) {
        fail();
        return false;
    }

    ControllerStationCommandType mapped_command{};
    if (!map_command(*pending, mapped_command)) {
        fail();
        return false;
    }
    const auto decision = station_state_.request_command(mapped_command);
    const auto result_status =
        decision.status == ControllerStationCommandDecisionStatus::Allow
            ? ControllerCommandResultStatus::Accepted
            : ControllerCommandResultStatus::Rejected;

    const auto result = session_->make_command_result(
        *pending, result_status,
        decision.reason_code.empty() ? "COMMAND_REJECTED" : decision.reason_code,
        sent_at);
    last_protocol_status_ = result.status;
    if (result.status != ControllerLinkPeerSessionStatus::Success ||
        !result.has_outbound_message ||
        result.outbound_message.type != ControllerMessageType::CommandResult ||
        !queue_outbound_message(result.outbound_message,
                                result.outbound_payload, false)) {
        fail();
        return false;
    }
    return true;
}

bool ControllerLinkPeerService::process_pending_plaintext(
    std::string_view sent_at) noexcept {
    std::size_t processed_frames = 0U;
    while (processed_frames < kMaximumFramesPerPoll) {
        if (decoder_.has_frame()) {
            if (!process_ready_frame(sent_at)) {
                return false;
            }
            ++processed_frames;
            if (outbound_frame_size_ != 0U) {
                return true;
            }
            continue;
        }

        if (inbound_offset_ >= inbound_size_) {
            inbound_size_ = 0U;
            inbound_offset_ = 0U;
            return true;
        }

        const auto progress = decoder_.push_bytes(
            inbound_buffer_.data() + inbound_offset_,
            inbound_size_ - inbound_offset_);
        inbound_offset_ += progress.bytes_consumed;

        if (progress.status ==
                hardware::interfaces::ControllerFrameStreamStatus::NullInput ||
            (progress.bytes_consumed == 0U && !decoder_.has_frame())) {
            fail();
            return false;
        }
    }
    return true;
}

bool ControllerLinkPeerService::process_ready_frame(
    std::string_view sent_at) noexcept {
    hardware::interfaces::ControllerCodecDecodedMessage decoded{};
    const auto result = hardware::interfaces::decode_controller_message(
        decoder_.frame_data(), decoder_.frame_size(), decode_storage_, decoded);

    if (result.status != ControllerCodecStatus::Success) {
        ++rejected_frames_;
        return decoder_.reject_frame();
    }

    const auto accepted = session_->handle_message(decoded, sent_at);
    last_protocol_status_ = accepted.status;
    ++messages_received_;

    if (!decoder_.consume_frame()) {
        fail();
        return false;
    }

    if (accepted.status != ControllerLinkPeerSessionStatus::Success) {
        fail();
        return false;
    }

    if (accepted.event == ControllerLinkPeerSessionEvent::SessionEstablished) {
        if (!accepted.has_outbound_message ||
            accepted.outbound_message.type !=
                hardware::interfaces::ControllerMessageType::HelloAck ||
            !queue_outbound_message(accepted.outbound_message,
                                    accepted.outbound_payload, true)) {
            fail();
            return false;
        }
        logical_handshake_ack_pending_ = true;
        return true;
    }

    if (accepted.event == ControllerLinkPeerSessionEvent::CommandRequested) {
        return handle_command_request(sent_at);
    }

    if (accepted.event == ControllerLinkPeerSessionEvent::SnapshotRequested) {
        if (!logical_session_established_) {
            fail();
            return false;
        }
        snapshot_request_pending_ = true;
    }

    return true;
}

bool ControllerLinkPeerService::queue_outbound_message(
    const hardware::interfaces::ControllerMessageEnvelope& envelope,
    const hardware::interfaces::ControllerMessagePayload& payload,
    bool is_hello_ack) noexcept {
    if (outbound_frame_size_ != 0U) {
        return false;
    }

    const auto encoded = hardware::interfaces::encode_controller_message(
        envelope, payload, outbound_frame_.data(), outbound_frame_.size());
    if (encoded.status != ControllerCodecStatus::Success) {
        return false;
    }

    outbound_frame_size_ = encoded.frame_size;
    outbound_frame_offset_ = 0U;
    logical_handshake_ack_pending_ = is_hello_ack;
    return true;
}

bool ControllerLinkPeerService::flush_outbound_message() noexcept {
    if (outbound_frame_size_ == 0U) {
        return true;
    }

    const auto sent = tls_server_.write_application_data(
        outbound_frame_.data() + outbound_frame_offset_,
        outbound_frame_size_ - outbound_frame_offset_);

    if (sent.status == ControllerMbedTlsIoStatus::WouldBlock) {
        // Retry the same write arguments on the next event-loop step after
        // the transport makes progress; do not call SSL_read meanwhile.
        return false;
    }
    if (sent.status != ControllerMbedTlsIoStatus::Success ||
        sent.bytes_transferred == 0U) {
        fail();
        return false;
    }

    outbound_frame_offset_ += sent.bytes_transferred;
    if (outbound_frame_offset_ < outbound_frame_size_) {
        return false;
    }

    mbedtls_platform_zeroize(outbound_frame_.data(), outbound_frame_.size());
    outbound_frame_size_ = 0U;
    outbound_frame_offset_ = 0U;

    if (logical_handshake_ack_pending_) {
        logical_handshake_ack_pending_ = false;
        logical_session_established_ = true;
        state_ = ControllerLinkPeerServiceState::Established;
    }

    return true;
}

bool ControllerLinkPeerService::send_telemetry_snapshot(
    const hardware::interfaces::ControllerTelemetrySnapshotPayload& snapshot,
    std::string_view sent_at) noexcept {
    if (state_ != ControllerLinkPeerServiceState::Established ||
        !logical_session_established_ || !snapshot_request_pending_ ||
        outbound_frame_size_ != 0U) {
        return false;
    }

    const auto message = session_->make_telemetry_snapshot(snapshot, sent_at);
    last_protocol_status_ = message.status;
    if (message.status != ControllerLinkPeerSessionStatus::Success) {
        // Invalid sensor data must not consume the request or close the link.
        return false;
    }
    if (!message.has_outbound_message ||
        message.outbound_message.type !=
            hardware::interfaces::ControllerMessageType::TelemetrySnapshot ||
        !queue_outbound_message(message.outbound_message,
                                message.outbound_payload, false)) {
        fail();
        return false;
    }

    snapshot_request_pending_ = false;
    ++telemetry_snapshots_sent_;
    return true;
}

bool ControllerLinkPeerService::export_authenticated_session_proof(
    std::array<std::uint8_t,
               hardware::interfaces::kControllerLinkSessionProofSize>&
        output) noexcept {
    mbedtls_platform_zeroize(output.data(), output.size());
    if (state_ != ControllerLinkPeerServiceState::Established ||
        !logical_session_established_ || outbound_frame_size_ != 0U) {
        return false;
    }
    return tls_server_.export_session_proof(output);
}

void ControllerLinkPeerService::update_lid_limit_switch_samples(
    std::optional<bool> open_line_high,
    std::optional<bool> closed_line_high,
    std::uint64_t monotonic_time_ms) noexcept {
    const auto current = station_state_.snapshot().safety;
    station_state_.set_safety_inputs(safety_input_adapter_.update(
        current, open_line_high, closed_line_high, monotonic_time_ms));
}

ControllerLinkPeerServiceStatus ControllerLinkPeerService::status() const noexcept {
    ControllerLinkPeerServiceStatus result{};
    result.state = state_;
    result.tls_authenticated =
        tls_server_.is_handshake_complete() &&
        (state_ == ControllerLinkPeerServiceState::AwaitingHello ||
         state_ == ControllerLinkPeerServiceState::Established);
    result.protocol_established = logical_session_established_;
    result.snapshot_request_pending = snapshot_request_pending_;
    result.outbound_message_pending = outbound_frame_size_ != 0U;
    result.messages_received = messages_received_;
    result.telemetry_snapshots_sent = telemetry_snapshots_sent_;
    result.rejected_frames = rejected_frames_;
    result.last_mbedtls_error = last_mbedtls_error_;
    result.last_protocol_status = last_protocol_status_;
    return result;
}

void ControllerLinkPeerService::stop() noexcept {
    tls_server_.close();
    last_mbedtls_error_ = 0;
    if (session_.has_value()) {
        session_->stop();
    }
    clear_connection_buffers();
    logical_handshake_ack_pending_ = false;
    logical_session_established_ = false;
    snapshot_request_pending_ = false;
    station_state_.clear_pending_command();
    state_ = ControllerLinkPeerServiceState::Stopped;
}

void ControllerLinkPeerService::fail() noexcept {
    last_mbedtls_error_ = tls_server_.last_mbedtls_error();
    tls_server_.close();
    if (session_.has_value()) {
        session_->stop();
    }
    clear_connection_buffers();
    logical_handshake_ack_pending_ = false;
    logical_session_established_ = false;
    snapshot_request_pending_ = false;
    station_state_.clear_pending_command();
    state_ = ControllerLinkPeerServiceState::Error;
}

void ControllerLinkPeerService::begin_tls_handshake() noexcept {
    last_mbedtls_error_ = 0;
    clear_connection_buffers();
    logical_session_established_ = false;
    logical_handshake_ack_pending_ = false;
    snapshot_request_pending_ = false;
    state_ = ControllerLinkPeerServiceState::TlsHandshake;
}

void ControllerLinkPeerService::clear_connection_buffers() noexcept {
    decoder_.reset();
    decode_storage_ = {};
    mbedtls_platform_zeroize(inbound_buffer_.data(), inbound_buffer_.size());
    inbound_size_ = 0U;
    inbound_offset_ = 0U;
    mbedtls_platform_zeroize(outbound_frame_.data(), outbound_frame_.size());
    outbound_frame_size_ = 0U;
    outbound_frame_offset_ = 0U;
}

}  // namespace dockstation::firmware::esp32
