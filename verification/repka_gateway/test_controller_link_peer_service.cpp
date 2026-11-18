#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ssl.h>
#include <openssl/crypto.h>

#include "controller_link_peer_service.hpp"
#include "hardware/interfaces/controller_codec.hpp"
#include "hardware/interfaces/controller_frame_stream_decoder.hpp"
#include "platform/linux/controller_psk_tls_client.hpp"

using namespace dockstation::firmware::esp32;
using namespace dockstation::platform::linux_platform;
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

struct SocketContext final {
    int fd{-1};
};

int socket_send(void* context, const unsigned char* bytes,
                std::size_t count) noexcept {
    if (context == nullptr || bytes == nullptr || count == 0U) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }

    const int fd = static_cast<SocketContext*>(context)->fd;
    for (;;) {
        const auto result = ::send(fd, bytes, count, MSG_NOSIGNAL);
        if (result > 0) {
            return static_cast<int>(result);
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
}

int socket_receive(void* context, unsigned char* bytes,
                   std::size_t capacity) noexcept {
    if (context == nullptr || bytes == nullptr || capacity == 0U) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }

    const int fd = static_cast<SocketContext*>(context)->fd;
    for (;;) {
        const auto result = ::recv(fd, bytes, capacity, 0);
        if (result >= 0) {
            return static_cast<int>(result);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void fill_credentials(ControllerMbedTlsPskCredentials& server,
                      ControllerPskCredentials& client) {
    for (std::size_t index = 0U; index < server.psk.size(); ++index) {
        server.psk[index] = static_cast<std::uint8_t>(index + 1U);
        client.psk[index] = server.psk[index];
    }
    for (std::size_t index = 0U; index < server.identity.size(); ++index) {
        server.identity[index] = static_cast<std::uint8_t>(0x40U + index);
        client.identity[index] = server.identity[index];
    }
}

bool flush_client_ciphertext(ControllerPskTlsClient& client, int fd) {
    std::array<std::uint8_t, 4096U> encrypted{};
    for (std::size_t attempt = 0U; attempt < 32U; ++attempt) {
        const auto taken = client.take_encrypted_bytes(
            encrypted.data(), encrypted.size());
        if (taken.status == ControllerTlsIoStatus::WouldBlock) {
            return true;
        }
        if (taken.status != ControllerTlsIoStatus::Success) {
            return false;
        }
        if (taken.bytes_transferred == 0U) {
            return true;
        }

        std::size_t offset = 0U;
        while (offset < taken.bytes_transferred) {
            const auto sent = ::send(
                fd, encrypted.data() + offset,
                taken.bytes_transferred - offset, MSG_NOSIGNAL);
            if (sent > 0) {
                offset += static_cast<std::size_t>(sent);
            } else if (sent < 0 && errno == EINTR) {
                continue;
            } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd descriptor{fd, POLLOUT, 0};
                if (::poll(&descriptor, 1U, 100) <= 0) {
                    return false;
                }
            } else {
                return false;
            }
        }
    }
    return true;
}

bool receive_client_ciphertext(ControllerPskTlsClient& client, int fd,
                               int timeout_ms) {
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1U, timeout_ms);
    if (ready == 0) {
        return true;
    }
    if (ready < 0) {
        return errno == EINTR;
    }
    if ((descriptor.revents & POLLIN) == 0 &&
        (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return false;
    }

    std::array<std::uint8_t, 4096U> encrypted{};
    const auto received = ::recv(fd, encrypted.data(), encrypted.size(), 0);
    if (received <= 0) {
        return false;
    }

    std::size_t offset = 0U;
    const auto received_size = static_cast<std::size_t>(received);
    while (offset < received_size) {
        const auto fed = client.feed_encrypted_bytes(
            encrypted.data() + offset, received_size - offset);
        offset += fed.bytes_transferred;
        if (fed.status == ControllerTlsIoStatus::Failed ||
            fed.status == ControllerTlsIoStatus::NotConfigured ||
            fed.status == ControllerTlsIoStatus::InvalidArgument) {
            return false;
        }
        if (fed.bytes_transferred == 0U &&
            fed.status == ControllerTlsIoStatus::WouldBlock) {
            return false;
        }
    }
    return true;
}

bool drive_client_handshake(ControllerPskTlsClient& client, int fd) {
    for (std::size_t attempt = 0U; attempt < 500U; ++attempt) {
        if (client.is_handshake_complete()) {
            return flush_client_ciphertext(client, fd);
        }

        const auto progress = client.advance_handshake();
        if (progress == ControllerTlsProgress::Failed ||
            progress == ControllerTlsProgress::NotConfigured) {
            return false;
        }
        if (!flush_client_ciphertext(client, fd)) {
            return false;
        }
        if (client.is_handshake_complete()) {
            return flush_client_ciphertext(client, fd);
        }
        if (!receive_client_ciphertext(client, fd, 20)) {
            return false;
        }
    }
    return client.is_handshake_complete();
}

bool write_client_application_data(
    ControllerPskTlsClient& client, int fd,
    const std::uint8_t* bytes, std::size_t byte_count) {
    std::size_t offset = 0U;
    for (std::size_t attempt = 0U;
         offset < byte_count && attempt < 200U; ++attempt) {
        const auto written = client.write_application_data(
            bytes + offset, byte_count - offset);
        if (written.bytes_transferred > 0U) {
            offset += written.bytes_transferred;
            if (!flush_client_ciphertext(client, fd)) {
                return false;
            }
        }
        if (written.status == ControllerTlsIoStatus::Success) {
            continue;
        }
        if (written.status != ControllerTlsIoStatus::WouldBlock ||
            !receive_client_ciphertext(client, fd, 20)) {
            return false;
        }
    }
    return offset == byte_count && flush_client_ciphertext(client, fd);
}

bool write_controller_message(
    ControllerPskTlsClient& client, int fd,
    const ControllerMessageEnvelope& envelope,
    const ControllerMessagePayload& payload) {
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize> frame{};
    const auto encoded = encode_controller_message(
        envelope, payload, frame.data(), frame.size());
    return encoded.status == ControllerCodecStatus::Success &&
        write_client_application_data(
            client, fd, frame.data(), encoded.frame_size);
}

bool read_controller_message(
    ControllerPskTlsClient& client, int fd,
    ControllerFrameStreamDecoder& decoder,
    ControllerCodecDecodeStorage& storage,
    ControllerCodecDecodedMessage& decoded) {
    std::array<std::uint8_t, 512U> plaintext{};
    for (std::size_t attempt = 0U; attempt < 500U; ++attempt) {
        if (decoder.has_frame()) {
            return decode_controller_message(
                decoder.frame_data(), decoder.frame_size(), storage, decoded).status ==
                ControllerCodecStatus::Success;
        }

        const auto received = client.read_application_data(
            plaintext.data(), plaintext.size());
        if (received.status == ControllerTlsIoStatus::Success) {
            std::size_t offset = 0U;
            while (offset < received.bytes_transferred) {
                const auto pushed = decoder.push_bytes(
                    plaintext.data() + offset,
                    received.bytes_transferred - offset);
                offset += pushed.bytes_consumed;
                if (decoder.has_frame()) {
                    return decode_controller_message(
                        decoder.frame_data(), decoder.frame_size(),
                        storage, decoded).status == ControllerCodecStatus::Success;
                }
                if (pushed.bytes_consumed == 0U) {
                    return false;
                }
            }
            continue;
        }

        if (received.status != ControllerTlsIoStatus::WouldBlock ||
            !receive_client_ciphertext(client, fd, 20)) {
            return false;
        }
    }
    return false;
}

ControllerLinkPeerSessionConfig make_peer_config() {
    ControllerLinkPeerSessionConfig config{};
    config.local_controller_id = "esp32-01";
    config.local_controller_instance_id = "esp32-boot-01";
    config.peer_controller_id = "repka-01";
    config.protocol_version = {1U, 1U};
    return config;
}

struct ServerResult final {
    std::atomic<bool> started{false};
    std::atomic<bool> tls_authenticated{false};
    std::atomic<bool> early_exporter_blocked{false};
    std::atomic<bool> protocol_established{false};
    std::atomic<bool> exporter_ready{false};
    std::atomic<bool> invalid_snapshot_preserved_request{false};
    std::atomic<bool> telemetry_flushed{false};
    std::atomic<bool> empty_snapshot_flushed{false};
    std::atomic<bool> state_snapshot_flushed{false};
    std::atomic<bool> command_pending_observed{false};
    std::atomic<bool> command_pending_cleared_after_disconnect{false};
    std::atomic<std::uint64_t> last_messages_received{0U};
    std::atomic<int> last_protocol_status{0};
    std::atomic<bool> last_outbound_pending{false};
    std::atomic<int> last_peer_state{0};
    std::atomic<bool> stop_requested{false};
    std::atomic<int> error{0};
    std::array<std::uint8_t, kControllerLinkSessionProofSize> exporter{};
};

void run_peer_service(
    int fd,
    const ControllerMbedTlsPskCredentials& credentials,
    ServerResult& result) {
    SocketContext transport{fd};
    ControllerLinkPeerService service;
    if (!service.start(credentials, make_peer_config(), &transport,
                       socket_send, socket_receive)) {
        result.error.store(service.status().last_mbedtls_error,
                           std::memory_order_release);
        return;
    }
    result.started.store(true, std::memory_order_release);

    bool checked_early_exporter = false;
    for (std::size_t attempt = 0U;
         attempt < 12000U &&
         !result.stop_requested.load(std::memory_order_acquire);
         ++attempt) {
        service.poll("1700000000000", 1700000000U);
        const auto status = service.status();
        result.last_messages_received.store(status.messages_received,
                                             std::memory_order_release);
        result.last_protocol_status.store(
            static_cast<int>(status.last_protocol_status),
            std::memory_order_release);
        result.last_outbound_pending.store(status.outbound_message_pending,
                                           std::memory_order_release);
        result.last_peer_state.store(static_cast<int>(status.state),
                                     std::memory_order_release);
        if (status.command_pending) {
            result.command_pending_observed.store(true, std::memory_order_release);
        }
        if ((status.state == ControllerLinkPeerServiceState::Stopped ||
             status.state == ControllerLinkPeerServiceState::Error) &&
            !status.command_pending) {
            result.command_pending_cleared_after_disconnect.store(
                true, std::memory_order_release);
            break;
        }

        if (status.state == ControllerLinkPeerServiceState::Error) {
            result.error.store(
                status.last_mbedtls_error != 0
                    ? status.last_mbedtls_error
                    : static_cast<int>(status.last_protocol_status),
                std::memory_order_release);
            break;
        }

        if (status.tls_authenticated && !status.protocol_established &&
            !checked_early_exporter) {
            std::array<std::uint8_t, kControllerLinkSessionProofSize> proof{};
            const bool exported =
                service.export_authenticated_session_proof(proof);
            std::uint8_t nonzero = 0U;
            for (const auto byte : proof) {
                nonzero = static_cast<std::uint8_t>(nonzero | byte);
            }
            result.early_exporter_blocked.store(
                !exported && nonzero == 0U, std::memory_order_release);
            checked_early_exporter = true;
            result.tls_authenticated.store(true, std::memory_order_release);
            OPENSSL_cleanse(proof.data(), proof.size());
        }

        if (status.protocol_established &&
            !result.protocol_established.load(std::memory_order_acquire)) {
            result.protocol_established.store(true, std::memory_order_release);
            if (service.export_authenticated_session_proof(result.exporter)) {
                result.exporter_ready.store(true, std::memory_order_release);
            }
        }

        if (status.snapshot_request_pending &&
            !status.outbound_message_pending) {
            if (status.telemetry_snapshots_sent == 0U) {
                const ControllerTelemetryMeasurement invalid_measurement{
                    "", 18.0, "degC", SensorQualityStatus::Ok, "1700000000000"};
                const ControllerTelemetrySnapshotPayload invalid_snapshot{
                    &invalid_measurement, 1U};

                const bool invalid_was_rejected =
                    !service.send_telemetry_snapshot(
                        invalid_snapshot, "1700000000200") &&
                    service.snapshot_request_pending() &&
                    service.status().state ==
                        ControllerLinkPeerServiceState::Established;
                result.invalid_snapshot_preserved_request.store(
                    invalid_was_rejected, std::memory_order_release);

                if (!invalid_was_rejected) {
                    result.error.store(-100, std::memory_order_release);
                    break;
                }

                const ControllerTelemetryMeasurement measurements[] = {
                    {"test.climate.temperature", 23.75, "degC",
                     SensorQualityStatus::Ok, "1700000000000"},
                    {"test.lid.closed", true, "bool",
                     SensorQualityStatus::NotConfigured, "1700000000000"}
                };
                const ControllerTelemetrySnapshotPayload snapshot{
                    measurements, sizeof(measurements) / sizeof(measurements[0])};

                if (!service.send_telemetry_snapshot(
                        snapshot, "1700000000200")) {
                    result.error.store(-101, std::memory_order_release);
                    break;
                }
            } else if (status.telemetry_snapshots_sent == 1U) {
                const ControllerTelemetrySnapshotPayload empty_snapshot{};
                if (!service.send_telemetry_snapshot(
                        empty_snapshot, "1700000000300")) {
                    result.error.store(-102, std::memory_order_release);
                    break;
                }
            } else if (!service.send_station_state_snapshot("1700000000400", 1700000000U)) {
                result.error.store(-103, std::memory_order_release);
                break;
            }
        }

        const auto updated = service.status();
        if (updated.telemetry_snapshots_sent == 1U &&
            !updated.outbound_message_pending) {
            result.telemetry_flushed.store(true, std::memory_order_release);
        }
        if (updated.telemetry_snapshots_sent == 2U &&
            !updated.outbound_message_pending) {
            result.empty_snapshot_flushed.store(true, std::memory_order_release);
        }
        if (updated.telemetry_snapshots_sent == 3U &&
            !updated.outbound_message_pending) {
            result.state_snapshot_flushed.store(true, std::memory_order_release);
        }

        (void)::poll(nullptr, 0U, 1);
    }

    service.stop();
}

bool run_complete_peer_service_flow() {
    int sockets[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        expect(false, "creates the POSIX socket pair");
        return false;
    }
    if (!set_nonblocking(sockets[1])) {
        ::close(sockets[0]);
        ::close(sockets[1]);
        expect(false, "makes the Mbed TLS server transport non-blocking");
        return false;
    }

    ControllerMbedTlsPskCredentials server_credentials{};
    ControllerPskCredentials client_credentials{};
    fill_credentials(server_credentials, client_credentials);

    ServerResult server_result{};
    std::thread server_thread(
        run_peer_service, sockets[1], std::cref(server_credentials),
        std::ref(server_result));

    ControllerPskTlsClient client;
    const bool initialized = client.initialize(client_credentials);
    const bool tls_handshake =
        initialized && drive_client_handshake(client, sockets[0]);

    ControllerMessageEnvelope hello{};
    hello.type = ControllerMessageType::Hello;
    hello.protocol_version = ControllerProtocolVersion{1U, 1U};
    hello.controller_id = "repka-01";
    hello.controller_instance_id = "repka-boot-01";
    hello.sequence_number = 1U;
    hello.sent_at = "1700000000000";
    const ControllerMessagePayload empty_payload{
        EmptyControllerMessagePayload{}};
    const bool hello_sent = tls_handshake &&
        write_controller_message(client, sockets[0], hello, empty_payload);

    ControllerFrameStreamDecoder decoder{};
    ControllerCodecDecodeStorage storage{};
    ControllerCodecDecodedMessage decoded{};
    const bool hello_ack_received = hello_sent &&
        read_controller_message(client, sockets[0], decoder, storage, decoded);

    const bool hello_ack_valid = hello_ack_received &&
        decoded.envelope.type == ControllerMessageType::HelloAck &&
        decoded.envelope.controller_id == "esp32-01" &&
        decoded.envelope.controller_instance_id == "esp32-boot-01" &&
        decoded.envelope.sequence_number == 1U;
    const bool hello_ack_consumed =
        hello_ack_valid && decoder.consume_frame();

    ControllerMessageEnvelope request{};
    request.type = ControllerMessageType::RequestSnapshot;
    request.protocol_version = ControllerProtocolVersion{1U, 1U};
    request.controller_id = "repka-01";
    request.controller_instance_id = "repka-boot-01";
    request.sequence_number = 2U;
    request.sent_at = "1700000000100";
    const bool request_sent = hello_ack_consumed &&
        write_controller_message(client, sockets[0], request, empty_payload);

    const bool telemetry_received = request_sent &&
        read_controller_message(client, sockets[0], decoder, storage, decoded);

    bool telemetry_valid = false;
    if (telemetry_received &&
        decoded.envelope.type == ControllerMessageType::TelemetrySnapshot &&
        decoded.envelope.controller_id == "esp32-01" &&
        decoded.envelope.controller_instance_id == "esp32-boot-01" &&
        decoded.envelope.sequence_number == 2U) {
        const auto* snapshot =
            std::get_if<ControllerTelemetrySnapshotPayload>(&decoded.payload);
        telemetry_valid = snapshot != nullptr &&
            snapshot->measurement_count == 2U &&
            snapshot->measurements[0].sensor_id ==
                "test.climate.temperature" &&
            std::get<double>(snapshot->measurements[0].value) == 23.75 &&
            snapshot->measurements[1].sensor_id == "test.lid.closed" &&
            std::get<bool>(snapshot->measurements[1].value);
    }

    const bool first_snapshot_consumed =
        telemetry_received && telemetry_valid && decoder.consume_frame();
    ControllerMessageEnvelope empty_request = request;
    empty_request.sequence_number = 3U;
    empty_request.sent_at = "1700000000300";
    const bool empty_request_sent = first_snapshot_consumed &&
        write_controller_message(client, sockets[0], empty_request, empty_payload);
    const bool empty_telemetry_received = empty_request_sent &&
        read_controller_message(client, sockets[0], decoder, storage, decoded);
    bool empty_telemetry_valid = false;
    if (empty_telemetry_received &&
        decoded.envelope.type == ControllerMessageType::TelemetrySnapshot &&
        decoded.envelope.controller_id == "esp32-01" &&
        decoded.envelope.controller_instance_id == "esp32-boot-01" &&
        decoded.envelope.sequence_number == 3U) {
        const auto* snapshot =
            std::get_if<ControllerTelemetrySnapshotPayload>(&decoded.payload);
        empty_telemetry_valid =
            snapshot != nullptr && snapshot->measurement_count == 0U;
    }
    const bool empty_telemetry_consumed =
        empty_telemetry_received && empty_telemetry_valid && decoder.consume_frame();

    ControllerMessageEnvelope state_request = request;
    state_request.sequence_number = 4U;
    state_request.sent_at = "1700000000400";
    const bool state_request_sent = empty_telemetry_consumed &&
        write_controller_message(client, sockets[0], state_request, empty_payload);
    const bool state_telemetry_received = state_request_sent &&
        read_controller_message(client, sockets[0], decoder, storage, decoded);
    bool state_telemetry_valid = false;
    if (state_telemetry_received &&
        decoded.envelope.type == ControllerMessageType::TelemetrySnapshot) {
        const auto* snapshot =
            std::get_if<ControllerTelemetrySnapshotPayload>(&decoded.payload);
        if (snapshot != nullptr && snapshot->measurement_count == 8U) {
            bool lid_not_configured = false;
            bool motion_unknown = false;
            bool stop_unknown = false;
            bool pending_none = false;
            for (std::size_t index = 0U; index < snapshot->measurement_count; ++index) {
                const auto& measurement = snapshot->measurements[index];
                if (measurement.sensor_id == "lid.position") {
                    lid_not_configured =
                        std::get<std::string_view>(measurement.value) == "NOT_CONFIGURED" &&
                        measurement.quality == SensorQualityStatus::NotConfigured;
                } else if (measurement.sensor_id == "platform.motion_active") {
                    motion_unknown =
                        std::get<std::string_view>(measurement.value) == "UNKNOWN" &&
                        measurement.quality == SensorQualityStatus::Unknown;
                } else if (measurement.sensor_id == "safety_stop.active") {
                    stop_unknown =
                        std::get<std::string_view>(measurement.value) == "UNKNOWN" &&
                        measurement.quality == SensorQualityStatus::Unknown;
                } else if (measurement.sensor_id == "command.pending") {
                    pending_none =
                        std::get<std::string_view>(measurement.value) == "NONE";
                }
            }
            state_telemetry_valid =
                lid_not_configured && motion_unknown && stop_unknown && pending_none;
        }
    }
    const bool state_telemetry_consumed =
        state_telemetry_received && state_telemetry_valid && decoder.consume_frame();

    const auto send_command = [&](ControllerCommandType command,
                                  std::uint64_t sequence,
                                  std::string_view sent_at) {
        ControllerMessageEnvelope command_request{};
        command_request.type = ControllerMessageType::CommandRequest;
        command_request.protocol_version = ControllerProtocolVersion{1U, 1U};
        command_request.controller_id = "repka-01";
        command_request.controller_instance_id = "repka-boot-01";
        command_request.sequence_number = sequence;
        command_request.sent_at = sent_at;
        const ControllerMessagePayload command_payload{
            ControllerCommandRequestPayload{command}};
        return write_controller_message(
            client, sockets[0], command_request, command_payload);
    };

    const bool accepted_command_sent = state_telemetry_consumed &&
        send_command(ControllerCommandType::StopMotion, 5U, "1700000000500");
    const bool accepted_command_received = accepted_command_sent &&
        read_controller_message(client, sockets[0], decoder, storage, decoded);
    const auto* accepted_payload = accepted_command_received
        ? std::get_if<ControllerCommandResultPayload>(&decoded.payload)
        : nullptr;
    const bool accepted_command_valid =
        accepted_payload != nullptr &&
        decoded.envelope.type == ControllerMessageType::CommandResult &&
        decoded.envelope.sequence_number == 5U &&
        accepted_payload->command == ControllerCommandType::StopMotion &&
        accepted_payload->status == ControllerCommandResultStatus::Accepted &&
        accepted_payload->reason_code == "ALLOWED";
    const bool accepted_command_consumed =
        accepted_command_valid && decoder.consume_frame();

    const bool rejected_command_sent = accepted_command_consumed &&
        send_command(ControllerCommandType::OpenLid, 6U, "1700000000600");
    const bool rejected_command_received = rejected_command_sent &&
        read_controller_message(client, sockets[0], decoder, storage, decoded);
    const auto* rejected_payload = rejected_command_received
        ? std::get_if<ControllerCommandResultPayload>(&decoded.payload)
        : nullptr;
    const bool rejected_command_valid =
        rejected_payload != nullptr &&
        decoded.envelope.type == ControllerMessageType::CommandResult &&
        decoded.envelope.sequence_number == 6U &&
        rejected_payload->command == ControllerCommandType::OpenLid &&
        rejected_payload->status == ControllerCommandResultStatus::Rejected &&
        rejected_payload->reason_code == "COMMAND_ALREADY_PENDING";
    const bool rejected_command_consumed =
        rejected_command_valid && decoder.consume_frame();

    // A final accepted STOP remains pending until the authenticated transport
    // is lost; the peer service must clear it rather than retain stale intent.
    const bool disconnect_command_sent = rejected_command_consumed &&
        send_command(ControllerCommandType::StopMotion, 7U, "1700000000700");
    const bool disconnect_command_received = disconnect_command_sent &&
        read_controller_message(client, sockets[0], decoder, storage, decoded);
    const auto* disconnect_payload = disconnect_command_received
        ? std::get_if<ControllerCommandResultPayload>(&decoded.payload)
        : nullptr;
    const bool disconnect_command_valid =
        disconnect_payload != nullptr &&
        decoded.envelope.type == ControllerMessageType::CommandResult &&
        decoded.envelope.sequence_number == 7U &&
        disconnect_payload->command == ControllerCommandType::StopMotion &&
        disconnect_payload->status == ControllerCommandResultStatus::Accepted &&
        disconnect_payload->reason_code == "ALLOWED";
    const bool disconnect_command_consumed =
        disconnect_command_valid && decoder.consume_frame();

    std::array<std::uint8_t, kControllerLinkSessionProofSize> client_exporter{};
    const bool client_exported = tls_handshake && client.export_session_proof(
        reinterpret_cast<const std::uint8_t*>(kControllerSessionProofContext),
        sizeof(kControllerSessionProofContext) - 1U, client_exporter);

    bool server_exporter_matches = false;
    bool server_telemetry_flushed = false;
    bool server_empty_snapshot_flushed = false;
    for (std::size_t attempt = 0U; attempt < 500U; ++attempt) {
        server_exporter_matches =
            server_result.exporter_ready.load(std::memory_order_acquire) &&
            client_exported &&
            server_result.exporter == client_exporter;
        server_telemetry_flushed =
            server_result.telemetry_flushed.load(std::memory_order_acquire);
        server_empty_snapshot_flushed =
            server_result.empty_snapshot_flushed.load(std::memory_order_acquire);
        if (server_empty_snapshot_flushed ||
            server_result.error.load(std::memory_order_acquire) != 0) {
            break;
        }
        (void)::poll(nullptr, 0U, 5);
    }

    expect(initialized && tls_handshake &&
               server_result.tls_authenticated.load(std::memory_order_acquire),
           "completes TLS 1.3 PSK-DHE before ControllerLink protocol processing");
    expect(hello_ack_valid &&
               server_result.protocol_established.load(std::memory_order_acquire),
           "accepts HELLO and sends a valid HELLO_ACK");
    expect(server_result.early_exporter_blocked.load(std::memory_order_acquire) &&
               server_exporter_matches,
           "blocks exporter before HELLO_ACK and then matches Repka exporter bytes");
    expect(server_result.invalid_snapshot_preserved_request.load(
               std::memory_order_acquire),
           "rejects invalid sensor data without consuming REQUEST_SNAPSHOT");
    expect(telemetry_valid && server_telemetry_flushed,
           "responds to REQUEST_SNAPSHOT with the caller-supplied TELEMETRY_SNAPSHOT");
    expect(empty_telemetry_valid && server_empty_snapshot_flushed,
           "accepts and transmits an explicitly empty snapshot without inventing measurements");
    expect(state_telemetry_valid &&
               server_result.state_snapshot_flushed.load(std::memory_order_acquire),
           "sends station-state telemetry with NOT_CONFIGURED and UNKNOWN input quality");
    expect(accepted_command_valid,
           "TLS peer accepts a fail-safe STOP command and returns ACCEPTED/PENDING");
    expect(rejected_command_valid,
           "TLS peer rejects a second normal command while STOP remains pending");
    expect(disconnect_command_valid && disconnect_command_consumed &&
               server_result.command_pending_observed.load(std::memory_order_acquire),
           "accepted command remains pending without claiming physical completion");
    client.close();
    (void)::shutdown(sockets[0], SHUT_RDWR);
    ::close(sockets[0]);
    server_thread.join();
    if (!accepted_command_valid || !rejected_command_valid ||
        !disconnect_command_valid) {
        std::cerr << "ControllerLink command diagnostic: sent="
                  << accepted_command_sent
                  << ", received=" << accepted_command_received
                  << ", accepted_valid=" << accepted_command_valid
                  << ", rejected_sent=" << rejected_command_sent
                  << ", rejected_received=" << rejected_command_received
                  << ", rejected_valid=" << rejected_command_valid
                  << ", disconnect_sent=" << disconnect_command_sent
                  << ", disconnect_received=" << disconnect_command_received
                  << ", disconnect_valid=" << disconnect_command_valid
                  << ", peer_error=" << server_result.error.load(
                         std::memory_order_acquire)
                  << ", messages_received=" << server_result.last_messages_received.load(
                         std::memory_order_acquire)
                  << ", protocol_status=" << server_result.last_protocol_status.load(
                         std::memory_order_acquire)
                  << ", outbound_pending=" << server_result.last_outbound_pending.load(
                         std::memory_order_acquire)
                  << ", peer_state=" << server_result.last_peer_state.load(
                         std::memory_order_acquire) << '\n';
    }
    expect(server_result.command_pending_cleared_after_disconnect.load(
               std::memory_order_acquire),
           "peer service clears pending command when the authenticated transport is lost");
    (void)::shutdown(sockets[1], SHUT_RDWR);
    ::close(sockets[1]);

    server_credentials.clear();
    OPENSSL_cleanse(client_exporter.data(), client_exporter.size());
    OPENSSL_cleanse(server_result.exporter.data(), server_result.exporter.size());

    return initialized && tls_handshake && hello_ack_valid &&
        server_result.tls_authenticated.load(std::memory_order_acquire) &&
        server_result.protocol_established.load(std::memory_order_acquire) &&
        server_result.early_exporter_blocked.load(std::memory_order_acquire) &&
        server_exporter_matches &&
        server_result.invalid_snapshot_preserved_request.load(
            std::memory_order_acquire) &&
        telemetry_valid && server_telemetry_flushed &&
        empty_telemetry_valid && server_empty_snapshot_flushed &&
        state_telemetry_valid &&
        server_result.state_snapshot_flushed.load(std::memory_order_acquire) &&
        accepted_command_valid && rejected_command_valid &&
        disconnect_command_valid && disconnect_command_consumed &&
        server_result.command_pending_observed.load(std::memory_order_acquire) &&
        server_result.command_pending_cleared_after_disconnect.load(
            std::memory_order_acquire) &&
        server_result.error.load(std::memory_order_acquire) == 0;
}

}  // namespace

int main() {
    expect(run_complete_peer_service_flow(),
           "passes the complete TLS + HELLO/HELLO_ACK + snapshot integration");
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
