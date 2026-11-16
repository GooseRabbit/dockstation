#define _XOPEN_SOURCE 600

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <unistd.h>

#include "communications/api_serialization.hpp"
#include "hardware/interfaces/controller_codec.hpp"
#include "platform/linux/controller_link_service.hpp"

using namespace dockstation::hardware::interfaces;
using namespace dockstation::platform::linux_platform;

namespace dockstation::platform::linux_platform {

// Narrow test-only access to the credential-loader dependency. The production
// constructor always uses load_controller_psk_credentials from the secure store.
class ControllerLinkServiceTestAccess final {
public:
    static void set_credential_loader(
        ControllerLinkService& service,
        decltype(&load_controller_psk_credentials) loader) noexcept {
        service.credential_loader_ = loader;
    }
};

}  // namespace dockstation::platform::linux_platform

namespace {
int checks = 0;
int failures = 0;
ControllerPskCredentials* test_credentials = nullptr;

void expect(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << description << '\n';
    } else {
        std::cerr << "FAIL - " << description << '\n';
        ++failures;
    }
}

ControllerPskCredentialFileStatus load_test_credentials(
    ControllerPskCredentials* output) noexcept {
    if (output == nullptr || test_credentials == nullptr) {
        return ControllerPskCredentialFileStatus::InvalidArgument;
    }
    *output = *test_credentials;
    return ControllerPskCredentialFileStatus::Success;
}

struct Pty final {
    int master{-1};
    std::string slave{};

    ~Pty() {
        if (master >= 0) {
            (void)::close(master);
        }
    }

    bool open() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            return false;
        }
        std::array<char, 256U> path{};
        if (::ptsname_r(master, path.data(), path.size()) != 0) {
            return false;
        }
        slave = path.data();
        return true;
    }
};

struct PeerCredentials final {
    ControllerPskCredentials value{};
};

int find_psk(SSL* ssl, const unsigned char* identity,
             std::size_t identity_size, SSL_SESSION** session) noexcept {
    if (ssl == nullptr || identity == nullptr || session == nullptr) {
        return 0;
    }
    auto* peer = static_cast<PeerCredentials*>(SSL_get_app_data(ssl));
    if (peer == nullptr || identity_size != peer->value.identity.size() ||
        CRYPTO_memcmp(identity, peer->value.identity.data(), identity_size) != 0) {
        return 0;
    }

    constexpr unsigned char suite_id[] = {0x13U, 0x01U};
    const SSL_CIPHER* cipher = SSL_CIPHER_find(ssl, suite_id);
    SSL_SESSION* value = SSL_SESSION_new();
    if (cipher == nullptr || value == nullptr ||
        SSL_SESSION_set1_master_key(value, peer->value.psk.data(),
                                    peer->value.psk.size()) != 1 ||
        SSL_SESSION_set_cipher(value, cipher) != 1 ||
        SSL_SESSION_set_protocol_version(value, TLS1_3_VERSION) != 1 ||
        SSL_SESSION_set_max_early_data(value, 0U) != 1) {
        SSL_SESSION_free(value);
        return 0;
    }
    *session = value;
    return 1;
}

bool make_credentials(ControllerPskCredentials& value) noexcept {
    return RAND_bytes(value.psk.data(), static_cast<int>(value.psk.size())) == 1 &&
           RAND_bytes(value.identity.data(),
                      static_cast<int>(value.identity.size())) == 1;
}

struct TlsPeer final {
    SSL_CTX* context{nullptr};
    SSL* ssl{nullptr};
    BIO* transport{nullptr};
    PeerCredentials credentials{};

    ~TlsPeer() {
        SSL_free(ssl);
        BIO_free(transport);
        SSL_CTX_free(context);
        credentials.value.clear();
    }

    bool initialize(const ControllerPskCredentials& pair) {
        credentials.value = pair;
        context = SSL_CTX_new(TLS_server_method());
        if (context == nullptr ||
            SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_ciphersuites(context, "TLS_AES_128_GCM_SHA256") != 1 ||
            SSL_CTX_set1_groups_list(context, "P-256") != 1 ||
            SSL_CTX_set_max_early_data(context, 0U) != 1 ||
            SSL_CTX_set_num_tickets(context, 0U) != 1) {
            return false;
        }
        SSL_CTX_set_options(context, SSL_OP_NO_TICKET);
        SSL_CTX_clear_options(context, SSL_OP_ALLOW_NO_DHE_KEX);
        SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
        SSL_CTX_set_psk_find_session_callback(context, &find_psk);

        ssl = SSL_new(context);
        if (ssl == nullptr) {
            return false;
        }
        SSL_set_app_data(ssl, &credentials);
        BIO* ssl_bio = nullptr;
        if (BIO_new_bio_pair(&ssl_bio, 0U, &transport, 0U) != 1 ||
            ssl_bio == nullptr || transport == nullptr) {
            BIO_free(ssl_bio);
            return false;
        }
        SSL_set_bio(ssl, ssl_bio, ssl_bio);
        SSL_set_accept_state(ssl);
        return true;
    }
};

bool write_all(int fd, const std::uint8_t* bytes, std::size_t count) noexcept {
    std::size_t offset = 0U;
    while (offset < count) {
        const ssize_t n = ::write(fd, bytes + offset, count - offset);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool uart_to_peer(int master, BIO* peer) noexcept {
    std::array<std::uint8_t, 1024U> bytes{};
    for (std::size_t i = 0U; i < 32U; ++i) {
        const ssize_t n = ::read(master, bytes.data(), bytes.size());
        if (n > 0) {
            std::size_t written = 0U;
            if (BIO_write_ex(peer, bytes.data(), static_cast<std::size_t>(n),
                             &written) != 1 ||
                written != static_cast<std::size_t>(n)) {
                return false;
            }
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        } else {
            return n == 0;
        }
    }
    return false;
}

bool peer_to_uart(BIO* peer, int master) noexcept {
    std::array<std::uint8_t, 1024U> bytes{};
    for (std::size_t i = 0U; i < 32U; ++i) {
        std::size_t n = 0U;
        if (BIO_read_ex(peer, bytes.data(), bytes.size(), &n) != 1) {
            return BIO_should_retry(peer) != 0;
        }
        if (n == 0U) {
            return true;
        }
        if (!write_all(master, bytes.data(), n)) {
            return false;
        }
    }
    return false;
}

bool complete_tls_handshake(
    ControllerLinkService& service, TlsPeer& peer, int master) noexcept {
    bool server_ready = false;
    for (std::size_t attempt = 0U; attempt < 256U; ++attempt) {
        service.poll();
        if (!uart_to_peer(master, peer.transport)) {
            return false;
        }
        if (!server_ready) {
            const int result = SSL_accept(peer.ssl);
            if (result == 1) {
                server_ready = true;
            } else {
                const int error = SSL_get_error(peer.ssl, result);
                if (error != SSL_ERROR_WANT_READ &&
                    error != SSL_ERROR_WANT_WRITE) {
                    return false;
                }
            }
        }
        if (!peer_to_uart(peer.transport, master)) {
            return false;
        }
        if (server_ready && service.status().tls_authenticated) {
            return true;
        }
    }
    return false;
}

bool read_service_message(
    ControllerLinkService& service, TlsPeer& peer, int master,
    ControllerCodecDecodeStorage& storage,
    ControllerCodecDecodedMessage& decoded) noexcept {
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize> frame{};
    for (std::size_t attempt = 0U; attempt < 256U; ++attempt) {
        service.poll();
        if (!uart_to_peer(master, peer.transport)) {
            return false;
        }
        std::size_t received = 0U;
        const int result = SSL_read_ex(
            peer.ssl, frame.data(), frame.size(), &received);
        if (result == 1) {
            return received > 0U &&
                decode_controller_message(frame.data(), received,
                                          storage, decoded).status ==
                    ControllerCodecStatus::Success;
        }
        const int error = SSL_get_error(peer.ssl, result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            return false;
        }
        if (!peer_to_uart(peer.transport, master)) {
            return false;
        }
    }
    return false;
}

bool send_peer_message(
    TlsPeer& peer, int master,
    const ControllerMessageEnvelope& envelope,
    const ControllerMessagePayload& payload) noexcept {
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize> frame{};
    const auto encoded = encode_controller_message(
        envelope, payload, frame.data(), frame.size());
    if (encoded.status != ControllerCodecStatus::Success ||
        encoded.frame_size == 0U) {
        return false;
    }
    std::size_t written = 0U;
    return SSL_write_ex(peer.ssl, frame.data(), encoded.frame_size, &written) == 1 &&
           written == encoded.frame_size &&
           peer_to_uart(peer.transport, master);
}

ControllerMessageEnvelope peer_envelope(
    ControllerMessageType type, std::uint64_t sequence) noexcept {
    ControllerMessageEnvelope envelope{};
    envelope.type = type;
    envelope.protocol_version = ControllerProtocolVersion{1U, 0U};
    envelope.controller_id = "esp32-01";
    envelope.controller_instance_id = "esp32-boot-01";
    envelope.sequence_number = sequence;
    envelope.sent_at = "2026-11-16T06:39:34Z";
    return envelope;
}

}  // namespace

int main() {
    ControllerLinkService service{};
    const auto not_configured = service.start({});
    expect(not_configured.state == ControllerLinkServiceState::NotConfigured &&
               !not_configured.tls_authenticated &&
               !not_configured.protocol_established,
           "an omitted optional controller link remains NOT_CONFIGURED");

    service.poll();
    expect(service.status().state == ControllerLinkServiceState::NotConfigured,
           "poll does not invent a link or hardware state when not configured");

    ControllerLinkServiceConfig partial{};
    partial.uart_device_path = "/dev/serial0";
    partial.local_controller_id = "repka-01";
    const auto partial_result = service.start(partial);
    expect(partial_result.state == ControllerLinkServiceState::Error &&
               partial_result.error_code != nullptr &&
               std::string(partial_result.error_code) ==
                   "CONTROLLER_CONFIGURATION_INVALID",
           "a partial controller identity configuration is rejected before device access");

    ControllerLinkServiceConfig invalid_path{};
    invalid_path.uart_device_path = "/tmp/tty";
    invalid_path.local_controller_id = "repka-01";
    invalid_path.peer_controller_id = "esp32-01";
    const auto path_result = service.start(invalid_path);
    expect(path_result.state == ControllerLinkServiceState::Error &&
               path_result.error_code != nullptr &&
               std::string(path_result.error_code) ==
                   "CONTROLLER_CONFIGURATION_INVALID",
           "a non-/dev UART path is rejected before credential or device access");

    service.stop();
    expect(service.status().state == ControllerLinkServiceState::Stopped,
           "stop clears optional controller link runtime state");

    // End-to-end host fixture: real ControllerLinkService, POSIX PTY, OpenSSL
    // TLS 1.3 PSK-DHE peer, protocol handshake and production codec/snapshot.
    ControllerPskCredentials credentials{};
    expect(make_credentials(credentials),
           "generates ephemeral fixture PSK/identity without persisting it");
    test_credentials = &credentials;

    Pty pty;
    expect(pty.open(), "creates PTY for the service-level TLS peer fixture");
    if (pty.master < 0 || pty.slave.empty()) {
        test_credentials = nullptr;
        return EXIT_FAILURE;
    }

    TlsPeer peer;
    expect(peer.initialize(credentials),
           "initializes a TLS 1.3 PSK-DHE test peer with the same ephemeral key");
    if (peer.ssl == nullptr) {
        test_credentials = nullptr;
        return EXIT_FAILURE;
    }

    ControllerLinkService e2e_service{};
    ControllerLinkServiceTestAccess::set_credential_loader(
        e2e_service, &load_test_credentials);
    const ControllerLinkServiceConfig config{
        pty.slave, "repka-01", "esp32-01"};
    const auto started = e2e_service.start(config);
    expect(started.state == ControllerLinkServiceState::TlsHandshake,
           "starts the production service over the PTY without pre-claiming TLS");

    expect(complete_tls_handshake(e2e_service, peer, pty.master) &&
               e2e_service.status().tls_authenticated,
           "authenticates the complete service-to-peer TLS 1.3 PSK-DHE handshake");

    ControllerCodecDecodeStorage peer_storage{};
    ControllerCodecDecodedMessage peer_message{};
    expect(read_service_message(e2e_service, peer, pty.master,
                                peer_storage, peer_message) &&
               peer_message.envelope.type == ControllerMessageType::Hello &&
               peer_message.envelope.controller_id == "repka-01",
           "sends HELLO through TLS after the transport authenticates");

    const auto hello_ack = peer_envelope(ControllerMessageType::HelloAck, 1U);
    expect(send_peer_message(peer, pty.master, hello_ack,
                             ControllerMessagePayload{EmptyControllerMessagePayload{}}),
           "sends a codec-valid HELLO_ACK through the encrypted PTY stream");

    peer_storage = {};
    peer_message = {};
    expect(read_service_message(e2e_service, peer, pty.master,
                                peer_storage, peer_message) &&
               peer_message.envelope.type == ControllerMessageType::RequestSnapshot &&
               e2e_service.status().protocol_established,
           "establishes the logical session and emits one REQUEST_SNAPSHOT");

    const ControllerTelemetryMeasurement fixture_measurement{
        "fixture.temperature", 21.5, "C",
        SensorQualityStatus::Ok, "2026-11-16T06:39:34Z"};
    const ControllerTelemetrySnapshotPayload telemetry_payload{
        &fixture_measurement, 1U};
    const auto telemetry_envelope =
        peer_envelope(ControllerMessageType::TelemetrySnapshot, 2U);
    expect(send_peer_message(peer, pty.master, telemetry_envelope,
                             ControllerMessagePayload{telemetry_payload}),
           "sends a fixture telemetry frame through the established TLS stream");

    for (std::size_t attempt = 0U;
         attempt < 64U && !e2e_service.telemetry_snapshot().available();
         ++attempt) {
        e2e_service.poll();
    }
    const auto& snapshot = e2e_service.telemetry_snapshot();
    ControllerTelemetryMeasurementView measurement{};
    expect(snapshot.available() && snapshot.sequence_number() == 2U &&
               snapshot.controller_id() == "esp32-01" &&
               snapshot.measurement_count() == 1U &&
               snapshot.measurement(0U, measurement) &&
               measurement.sensor_id == "fixture.temperature" &&
               measurement.unit == "C" &&
               measurement.quality == SensorQualityStatus::Ok &&
               std::get_if<double>(&measurement.value) != nullptr &&
               *std::get_if<double>(&measurement.value) == 21.5,
           "retains an owned typed telemetry snapshot after the TLS frame buffer is reused");

    const auto established_link_status = e2e_service.status();
    const auto diagnostics = dockstation::communications::serialize_system_diagnostics(
        &established_link_status, nullptr, &snapshot);
    expect(diagnostics.find(
               "\"controller_telemetry\":{\"status\":\"PASS\"") != std::string::npos &&
               diagnostics.find("\"source_controller_id\":\"esp32-01\"") !=
                   std::string::npos &&
               diagnostics.find("\"sensor_id\":\"fixture.temperature\"") !=
                   std::string::npos &&
               diagnostics.find("\"value\":21.5") != std::string::npos,
           "publishes the accepted peer snapshot through the real diagnostics serializer");

    // A duplicate peer sequence is rejected by ControllerLinkSession before
    // replacing the last known good telemetry snapshot.
    expect(send_peer_message(peer, pty.master, telemetry_envelope,
                             ControllerMessagePayload{telemetry_payload}),
           "sends a deliberately duplicated telemetry sequence as a negative fixture");
    e2e_service.poll();
    expect(e2e_service.status().state == ControllerLinkServiceState::LinkLost &&
               e2e_service.status().rejected_frames == 1U &&
               e2e_service.telemetry_snapshot().available() &&
               e2e_service.telemetry_snapshot().sequence_number() == 2U,
           "rejects the replayed sequence, closes the link and retains the last accepted snapshot");

    const auto disconnected_link_status = e2e_service.status();
    const auto disconnected_diagnostics =
        dockstation::communications::serialize_system_diagnostics(
            &disconnected_link_status, nullptr, &e2e_service.telemetry_snapshot());
    expect(disconnected_diagnostics.find(
               "\"controller_telemetry\":{\"status\":\"WARN\"") !=
               std::string::npos,
           "reports retained telemetry as WARN after the authenticated link is lost");

    e2e_service.stop();
    expect(!e2e_service.telemetry_snapshot().available(),
           "explicit service stop clears the process-local telemetry snapshot");

    test_credentials = nullptr;
    credentials.clear();

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
