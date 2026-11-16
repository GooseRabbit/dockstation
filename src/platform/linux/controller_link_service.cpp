#include "platform/linux/controller_link_service.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include "hardware/interfaces/controller_codec.hpp"
#include "platform/linux/controller_psk_credential_store.hpp"

namespace dockstation::platform::linux_platform {
namespace {

using hardware::interfaces::ControllerCodecDecodeStorage;
using hardware::interfaces::ControllerCodecDecodedMessage;
using hardware::interfaces::ControllerCodecStatus;
using hardware::interfaces::ControllerLinkSessionConfig;
using hardware::interfaces::ControllerLinkSessionEvent;
using hardware::interfaces::ControllerLinkSessionResult;
using hardware::interfaces::ControllerLinkSessionState;
using hardware::interfaces::ControllerMessagePayload;
using hardware::interfaces::ControllerMessageType;
using hardware::interfaces::EmptyControllerMessagePayload;

constexpr std::uint64_t kReconnectDelayMilliseconds = 2000U;
constexpr std::size_t kApplicationReadBufferSize = 1024U;
constexpr std::size_t kMaximumReadsPerPoll = 8U;
constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] std::uint64_t monotonic_milliseconds() noexcept {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    const auto value =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint64_t unix_epoch_milliseconds() noexcept {
    const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
    const auto value =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

// The controller contract currently treats sent_at as opaque validated text.
// Until a shared timestamp encoding is approved, the Repka sender uses Unix
// epoch milliseconds in decimal form; this value is not used for deadlines.
[[nodiscard]] std::string_view make_timestamp(
    std::array<char, 32U>& storage) noexcept {
    const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
    const auto value =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const auto converted = std::to_chars(
        storage.data(), storage.data() + storage.size(), value, 10);
    if (converted.ec != std::errc{}) {
        return {};
    }
    return {storage.data(),
            static_cast<std::size_t>(converted.ptr - storage.data())};
}

[[nodiscard]] bool valid_controller_uart_path(std::string_view path) noexcept {
    if (path.size() < 6U || path.size() > 255U ||
        path.substr(0U, 5U) != "/dev/" ||
        path.find(static_cast<char>(0)) != std::string_view::npos ||
        path.back() == '/') {
        return false;
    }

    std::size_t offset = 5U;
    while (offset < path.size()) {
        const std::size_t separator = path.find('/', offset);
        const std::size_t end = separator == std::string_view::npos
            ? path.size() : separator;
        const std::string_view component = path.substr(offset, end - offset);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        for (const unsigned char ch : component) {
            const bool alpha = (ch >= 'A' && ch <= 'Z') ||
                               (ch >= 'a' && ch <= 'z');
            const bool digit = ch >= '0' && ch <= '9';
            if (!alpha && !digit && ch != '_' && ch != '-' && ch != '.') {
                return false;
            }
        }
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }
    return true;
}

[[nodiscard]] bool copy_wire_identifier(
    std::array<char,
        hardware::interfaces::kControllerLinkSessionMaximumIdentifierBytes>& target,
    std::size_t& target_size,
    std::string_view source) noexcept {
    if (source.empty() ||
        source.size() > target.size() ||
        !hardware::interfaces::is_valid_controller_wire_text(source)) {
        return false;
    }
    std::copy(source.begin(), source.end(), target.begin());
    target_size = source.size();
    return true;
}

[[nodiscard]] std::string_view identifier_view(
    const std::array<char,
        hardware::interfaces::kControllerLinkSessionMaximumIdentifierBytes>& value,
    std::size_t size) noexcept {
    return {value.data(), size};
}

[[nodiscard]] bool generate_instance_id(
    std::array<char, 32U>& destination) noexcept {
    std::array<unsigned char, 16U> random_bytes{};
    if (RAND_bytes(random_bytes.data(),
                   static_cast<int>(random_bytes.size())) != 1) {
        OPENSSL_cleanse(random_bytes.data(), random_bytes.size());
        return false;
    }
    for (std::size_t index = 0U; index < random_bytes.size(); ++index) {
        destination[index * 2U] =
            kHexDigits[(random_bytes[index] >> 4U) & 0x0FU];
        destination[index * 2U + 1U] =
            kHexDigits[random_bytes[index] & 0x0FU];
    }
    OPENSSL_cleanse(random_bytes.data(), random_bytes.size());
    return true;
}

[[nodiscard]] bool transport_failed(ControllerTransportStatus status) noexcept {
    return status == ControllerTransportStatus::Disconnected ||
           status == ControllerTransportStatus::PeerClosed ||
           status == ControllerTransportStatus::TlsFailed ||
           status == ControllerTransportStatus::TransportFailed ||
           status == ControllerTransportStatus::InvalidArgument;
}

[[nodiscard]] const char* credential_error_code(
    ControllerPskCredentialFileStatus status) noexcept {
    switch (status) {
        case ControllerPskCredentialFileStatus::Success:
            return "";
        case ControllerPskCredentialFileStatus::DirectoryUnavailable:
        case ControllerPskCredentialFileStatus::CredentialFileUnavailable:
            return "CREDENTIALS_NOT_PROVISIONED";
        case ControllerPskCredentialFileStatus::InvalidArgument:
            return "CREDENTIAL_PATH_INVALID";
        case ControllerPskCredentialFileStatus::DirectoryPermissionsInvalid:
        case ControllerPskCredentialFileStatus::CredentialFilePermissionsInvalid:
            return "CREDENTIAL_PERMISSIONS_INVALID";
        case ControllerPskCredentialFileStatus::CredentialFileFormatInvalid:
            return "CREDENTIAL_FORMAT_INVALID";
        case ControllerPskCredentialFileStatus::CredentialFileReadFailed:
            return "CREDENTIAL_READ_FAILED";
    }
    return "CREDENTIAL_STORE_ERROR";
}

}  // namespace

ControllerLinkService::~ControllerLinkService() {
    stop();
}

ControllerLinkServiceStatus ControllerLinkService::start(
    const ControllerLinkServiceConfig& config) noexcept {
    stop();

    const bool all_empty = config.uart_device_path.empty() &&
                           config.local_controller_id.empty() &&
                           config.peer_controller_id.empty();
    if (all_empty) {
        state_ = ControllerLinkServiceState::NotConfigured;
        error_code_ = "";
        return status();
    }

    if (!copy_configuration(config)) {
        state_ = ControllerLinkServiceState::Error;
        error_code_ = "CONTROLLER_CONFIGURATION_INVALID";
        return status();
    }

    if (!generate_instance_id(local_instance_id_)) {
        state_ = ControllerLinkServiceState::Error;
        error_code_ = "INSTANCE_ID_GENERATION_FAILED";
        return status();
    }

    if (!create_session()) {
        state_ = ControllerLinkServiceState::Error;
        error_code_ = "SESSION_CONFIGURATION_INVALID";
        return status();
    }

    configured_ = true;
    state_ = ControllerLinkServiceState::NotConfigured;
    attempt_transport_start(monotonic_milliseconds());
    return status();
}

void ControllerLinkService::poll() noexcept {
    if (!configured_ || state_ == ControllerLinkServiceState::Error ||
        state_ == ControllerLinkServiceState::Stopped) {
        return;
    }

    const std::uint64_t now_ms = monotonic_milliseconds();
    if (!transport_.is_started()) {
        if (now_ms >= next_retry_ms_) {
            attempt_transport_start(now_ms);
        }
        if (!transport_.is_started()) {
            return;
        }
    }

    const ControllerTransportResult progress = transport_.poll();
    if (transport_failed(progress.status)) {
        fail_transport("CONTROLLER_TRANSPORT_FAILED", now_ms);
        return;
    }

    if (!tls_authenticated_ && transport_.is_handshake_complete()) {
        std::array<char, 32U> timestamp_buffer{};
        const std::string_view sent_at = make_timestamp(timestamp_buffer);
        if (!begin_protocol_session(now_ms, sent_at)) {
            fail_transport("CONTROLLER_PROTOCOL_START_FAILED", now_ms);
            return;
        }
    }

    if (!tls_authenticated_) {
        return;
    }

    std::array<std::uint8_t, kApplicationReadBufferSize> inbound{};
    for (std::size_t attempt = 0U; attempt < kMaximumReadsPerPoll; ++attempt) {
        const auto received = transport_.read_application_data(
            inbound.data(), inbound.size());
        if (received.status == ControllerTlsIoStatus::WouldBlock) {
            break;
        }
        if (received.status != ControllerTlsIoStatus::Success ||
            received.bytes_transferred > inbound.size()) {
            fail_transport("CONTROLLER_TLS_READ_FAILED", now_ms);
            return;
        }
        if (received.bytes_transferred == 0U) {
            break;
        }
        if (!process_plaintext(inbound.data(), received.bytes_transferred, now_ms)) {
            return;
        }
    }

    std::array<char, 32U> timestamp_buffer{};
    const std::string_view sent_at = make_timestamp(timestamp_buffer);
    if (sent_at.empty()) {
        fail_transport("CONTROLLER_TIMESTAMP_UNAVAILABLE", now_ms);
        return;
    }

    if (snapshot_request_pending_ && outbound_frame_size_ == 0U) {
        const ControllerLinkSessionResult request =
            session_->request_snapshot(now_ms, sent_at);
        if (request.status != hardware::interfaces::ControllerLinkSessionStatus::Success ||
            !request.has_outbound_message ||
            !queue_outbound_message(request.outbound_message)) {
            fail_transport("CONTROLLER_SNAPSHOT_REQUEST_FAILED", now_ms);
            return;
        }
        snapshot_request_pending_ = false;
    }

    if (outbound_frame_size_ == 0U) {
        const ControllerLinkSessionResult tick = session_->poll(now_ms, sent_at);
        if (tick.status ==
                hardware::interfaces::ControllerLinkSessionStatus::HandshakeTimeout ||
            tick.status ==
                hardware::interfaces::ControllerLinkSessionStatus::HeartbeatTimeout) {
            state_ = ControllerLinkServiceState::LinkLost;
        } else if (tick.status ==
                   hardware::interfaces::ControllerLinkSessionStatus::Success &&
                   tick.has_outbound_message) {
            if (!queue_outbound_message(tick.outbound_message)) {
                fail_transport("CONTROLLER_OUTBOUND_FRAME_FAILED", now_ms);
                return;
            }
        } else if (tick.status !=
                   hardware::interfaces::ControllerLinkSessionStatus::Success) {
            fail_transport("CONTROLLER_SESSION_FAILED", now_ms);
            return;
        }
    }

    update_protocol_state();
    if (!flush_outbound_message()) {
        fail_transport("CONTROLLER_TLS_WRITE_FAILED", now_ms);
        return;
    }

    const ControllerTransportResult flushed = transport_.poll();
    if (transport_failed(flushed.status)) {
        fail_transport("CONTROLLER_TRANSPORT_FAILED", now_ms);
    }
}

void ControllerLinkService::stop() noexcept {
    transport_.close();
    if (session_.has_value()) {
        session_->stop();
        session_.reset();
    }
    decoder_.reset();
    telemetry_snapshot_.clear();
    OPENSSL_cleanse(outbound_frame_.data(), outbound_frame_.size());
    OPENSSL_cleanse(local_instance_id_.data(), local_instance_id_.size());
    outbound_frame_size_ = 0U;
    outbound_frame_offset_ = 0U;
    next_retry_ms_ = 0U;
    telemetry_snapshots_received_ = 0U;
    error_messages_received_ = 0U;
    rejected_frames_ = 0U;
    configured_ = false;
    tls_authenticated_ = false;
    snapshot_request_pending_ = false;
    local_controller_id_size_ = 0U;
    peer_controller_id_size_ = 0U;
    uart_device_path_.fill('\0');
    local_controller_id_.fill('\0');
    peer_controller_id_.fill('\0');
    state_ = ControllerLinkServiceState::Stopped;
    error_code_ = "";
}

ControllerLinkServiceStatus ControllerLinkService::status() const noexcept {
    const bool protocol_established =
        session_.has_value() &&
        session_->state() == ControllerLinkSessionState::Established;
    return {state_, tls_authenticated_, protocol_established,
            telemetry_snapshots_received_, error_messages_received_,
            rejected_frames_, error_code_};
}

bool ControllerLinkService::copy_configuration(
    const ControllerLinkServiceConfig& config) noexcept {
    if (!valid_controller_uart_path(config.uart_device_path) ||
        config.uart_device_path.size() >= uart_device_path_.size()) {
        return false;
    }
    std::copy(config.uart_device_path.begin(), config.uart_device_path.end(),
              uart_device_path_.begin());

    if (!copy_wire_identifier(local_controller_id_,
                              local_controller_id_size_,
                              config.local_controller_id) ||
        !copy_wire_identifier(peer_controller_id_,
                              peer_controller_id_size_,
                              config.peer_controller_id) ||
        identifier_view(local_controller_id_, local_controller_id_size_) ==
            identifier_view(peer_controller_id_, peer_controller_id_size_)) {
        return false;
    }
    return true;
}

bool ControllerLinkService::create_session() noexcept {
    ControllerLinkSessionConfig config{};
    config.local_controller_id =
        identifier_view(local_controller_id_, local_controller_id_size_);
    config.local_controller_instance_id =
        std::string_view{local_instance_id_.data(), local_instance_id_.size()};
    config.peer_controller_id =
        identifier_view(peer_controller_id_, peer_controller_id_size_);
    session_.emplace(config);
    return session_->is_configured();
}

void ControllerLinkService::attempt_transport_start(
    std::uint64_t now_ms) noexcept {
    ControllerPskCredentials credentials{};
    const ControllerPskCredentialFileStatus credential_status =
        load_controller_psk_credentials(&credentials);
    if (credential_status != ControllerPskCredentialFileStatus::Success) {
        credentials.clear();
        error_code_ = credential_error_code(credential_status);
        if (credential_status ==
                ControllerPskCredentialFileStatus::DirectoryUnavailable ||
            credential_status ==
                ControllerPskCredentialFileStatus::CredentialFileUnavailable) {
            state_ = ControllerLinkServiceState::NotConfigured;
            next_retry_ms_ = now_ms + kReconnectDelayMilliseconds;
        } else {
            state_ = ControllerLinkServiceState::Error;
        }
        return;
    }

    const ControllerTransportResult result =
        transport_.start(uart_device_path_.data(), credentials);
    credentials.clear();
    if (result.status != ControllerTransportStatus::Started) {
        state_ = ControllerLinkServiceState::LinkLost;
        error_code_ = "CONTROLLER_UART_OPEN_FAILED";
        next_retry_ms_ = now_ms + kReconnectDelayMilliseconds;
        tls_authenticated_ = false;
        snapshot_request_pending_ = false;
        decoder_.reset();
        return;
    }

    state_ = ControllerLinkServiceState::TlsHandshake;
    error_code_ = "";
    tls_authenticated_ = false;
    snapshot_request_pending_ = false;
    outbound_frame_size_ = 0U;
    outbound_frame_offset_ = 0U;
    decoder_.reset();
}

void ControllerLinkService::fail_transport(
    const char* error_code, std::uint64_t now_ms) noexcept {
    transport_.close();
    if (session_.has_value()) {
        session_->stop();
    }
    decoder_.reset();
    OPENSSL_cleanse(outbound_frame_.data(), outbound_frame_.size());
    outbound_frame_size_ = 0U;
    outbound_frame_offset_ = 0U;
    snapshot_request_pending_ = false;
    tls_authenticated_ = false;
    state_ = ControllerLinkServiceState::LinkLost;
    error_code_ = error_code;
    next_retry_ms_ = now_ms + kReconnectDelayMilliseconds;
}

bool ControllerLinkService::queue_outbound_message(
    const hardware::interfaces::ControllerMessageEnvelope& envelope) noexcept {
    if (outbound_frame_size_ != 0U) {
        return false;
    }
    const auto result = hardware::interfaces::encode_controller_message(
        envelope,
        ControllerMessagePayload{EmptyControllerMessagePayload{}},
        outbound_frame_.data(), outbound_frame_.size());
    if (result.status != ControllerCodecStatus::Success ||
        result.frame_size == 0U ||
        result.frame_size > outbound_frame_.size()) {
        return false;
    }
    outbound_frame_size_ = result.frame_size;
    outbound_frame_offset_ = 0U;
    return true;
}

bool ControllerLinkService::flush_outbound_message() noexcept {
    if (outbound_frame_size_ == 0U) {
        return true;
    }
    if (outbound_frame_offset_ > outbound_frame_size_) {
        return false;
    }

    const std::size_t remaining = outbound_frame_size_ - outbound_frame_offset_;
    const auto sent = transport_.write_application_data(
        outbound_frame_.data() + outbound_frame_offset_, remaining);
    if (sent.status == ControllerTlsIoStatus::WouldBlock) {
        return true;
    }
    if (sent.status != ControllerTlsIoStatus::Success ||
        sent.bytes_transferred > remaining) {
        return false;
    }
    outbound_frame_offset_ += sent.bytes_transferred;
    if (outbound_frame_offset_ == outbound_frame_size_) {
        OPENSSL_cleanse(outbound_frame_.data(), outbound_frame_.size());
        outbound_frame_size_ = 0U;
        outbound_frame_offset_ = 0U;
    }
    return true;
}

bool ControllerLinkService::process_plaintext(
    const std::uint8_t* bytes, std::size_t byte_count,
    std::uint64_t now_ms) noexcept {
    if (bytes == nullptr && byte_count != 0U) {
        fail_transport("CONTROLLER_NULL_APPLICATION_DATA", now_ms);
        return false;
    }

    std::size_t offset = 0U;
    while (offset < byte_count) {
        if (decoder_.has_frame()) {
            if (!process_ready_frame(now_ms)) {
                return false;
            }
            continue;
        }
        const auto pushed = decoder_.push_bytes(bytes + offset, byte_count - offset);
        if (pushed.status ==
            hardware::interfaces::ControllerFrameStreamStatus::NullInput) {
            fail_transport("CONTROLLER_FRAME_STREAM_INVALID", now_ms);
            return false;
        }
        offset += pushed.bytes_consumed;
        if (decoder_.has_frame()) {
            if (!process_ready_frame(now_ms)) {
                return false;
            }
            continue;
        }
        if (pushed.bytes_consumed == 0U) {
            break;
        }
    }

    while (decoder_.has_frame()) {
        if (!process_ready_frame(now_ms)) {
            return false;
        }
    }
    return true;
}

bool ControllerLinkService::process_ready_frame(std::uint64_t now_ms) noexcept {
    if (!session_.has_value() || !decoder_.has_frame()) {
        return false;
    }

    ControllerCodecDecodeStorage storage{};
    ControllerCodecDecodedMessage decoded{};
    const auto decoded_result = hardware::interfaces::decode_controller_message(
        decoder_.frame_data(), decoder_.frame_size(), storage, decoded);
    if (decoded_result.status != ControllerCodecStatus::Success) {
        ++rejected_frames_;
        fail_transport("CONTROLLER_FRAME_REJECTED", now_ms);
        return false;
    }

    const auto accepted = session_->handle_message(decoded, now_ms);
    if (accepted.status !=
        hardware::interfaces::ControllerLinkSessionStatus::Success) {
        ++rejected_frames_;
        fail_transport("CONTROLLER_MESSAGE_REJECTED", now_ms);
        return false;
    }

    if (decoded.envelope.type == ControllerMessageType::TelemetrySnapshot) {
        if (!telemetry_snapshot_.replace(decoded, unix_epoch_milliseconds())) {
            ++rejected_frames_;
            fail_transport("CONTROLLER_TELEMETRY_SNAPSHOT_REJECTED", now_ms);
            return false;
        }
        ++telemetry_snapshots_received_;
    } else if (decoded.envelope.type == ControllerMessageType::Error) {
        ++error_messages_received_;
    }
    if (accepted.event == ControllerLinkSessionEvent::SessionEstablished) {
        snapshot_request_pending_ = true;
        state_ = ControllerLinkServiceState::Established;
    }

    if (!decoder_.consume_frame()) {
        fail_transport("CONTROLLER_FRAME_CONSUME_FAILED", now_ms);
        return false;
    }
    return true;
}

bool ControllerLinkService::begin_protocol_session(
    std::uint64_t now_ms, std::string_view sent_at) noexcept {
    if (!session_.has_value() || sent_at.empty()) {
        return false;
    }
    const ControllerLinkSessionResult hello = session_->start(now_ms, sent_at);
    if (hello.status !=
            hardware::interfaces::ControllerLinkSessionStatus::Success ||
        !hello.has_outbound_message ||
        hello.outbound_message.type != ControllerMessageType::Hello ||
        !queue_outbound_message(hello.outbound_message)) {
        return false;
    }
    tls_authenticated_ = true;
    state_ = ControllerLinkServiceState::ProtocolHandshake;
    error_code_ = "";
    return true;
}

void ControllerLinkService::update_protocol_state() noexcept {
    if (!tls_authenticated_ || !session_.has_value()) {
        return;
    }
    switch (session_->state()) {
        case ControllerLinkSessionState::Stopped:
            state_ = ControllerLinkServiceState::TlsHandshake;
            break;
        case ControllerLinkSessionState::AwaitingHelloAck:
            state_ = ControllerLinkServiceState::ProtocolHandshake;
            break;
        case ControllerLinkSessionState::Established:
            state_ = ControllerLinkServiceState::Established;
            break;
        case ControllerLinkSessionState::LinkLost:
            state_ = ControllerLinkServiceState::LinkLost;
            break;
    }
}

}  // namespace dockstation::platform::linux_platform
