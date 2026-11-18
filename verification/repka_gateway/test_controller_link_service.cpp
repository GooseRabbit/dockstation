#define _XOPEN_SOURCE 600

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <string>
#include <thread>

#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <unistd.h>

#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
#include <sqlite3.h>
#include "platform/persistence/sqlite_database.hpp"
#include "platform/persistence/sqlite_source_session_store.hpp"
#include "platform/persistence/sqlite_subsystem_status_store.hpp"
#endif

#include "communications/api_serialization.hpp"
#include "communications/authenticated_event_transport_adapter.hpp"
#include "communications/controller_subsystem_status_event_projector.hpp"
#include "core/station_event_pipeline.hpp"
#include "hardware/interfaces/controller_codec.hpp"
#include "platform/linux/controller_link_service.hpp"
#include "platform/logging/logger.hpp"

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

    ~TlsPeer() { reset(); }

    void reset() noexcept {
        SSL_free(ssl);
        ssl = nullptr;
        BIO_free(transport);
        transport = nullptr;
        SSL_CTX_free(context);
        context = nullptr;
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
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize>& frame,
    ControllerCodecDecodeStorage& storage,
    ControllerCodecDecodedMessage& decoded) noexcept {
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

#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
bool query_integer(sqlite3* database, const char* sql, sqlite3_int64& value) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    bool valid = false;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int64(statement, 0);
        valid = sqlite3_step(statement) == SQLITE_DONE;
    }
    return sqlite3_finalize(statement) == SQLITE_OK && valid;
}

bool query_text(sqlite3* database, const char* sql, std::string& value) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    bool valid = false;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* bytes = sqlite3_column_text(statement, 0);
        const int length = sqlite3_column_bytes(statement, 0);
        if (bytes != nullptr && length > 0) {
            value.assign(reinterpret_cast<const char*>(bytes),
                         static_cast<std::size_t>(length));
            valid = sqlite3_step(statement) == SQLITE_DONE;
        }
    }
    return sqlite3_finalize(statement) == SQLITE_OK && valid;
}
#endif

ControllerMessageEnvelope peer_envelope(
    ControllerMessageType type, std::uint64_t sequence) noexcept {
    ControllerMessageEnvelope envelope{};
    envelope.type = type;
    envelope.protocol_version = ControllerProtocolVersion{1U, 1U};
    envelope.controller_id = "esp32-01";
    envelope.controller_instance_id = "esp32-boot-01";
    envelope.sequence_number = sequence;
    envelope.sent_at = "2026-11-16T06:39:34Z";
    return envelope;
}

class CaptureVerifier final : public dockstation::core::SourceSessionVerifier {
public:
    int calls{0};
    std::string source{};
    std::string source_instance{};
    std::string peer_identity{};
    std::string session_proof{};

    dockstation::core::SourceSessionVerificationResult verify_source_session(
        const dockstation::core::SourceSessionEvidence& evidence) noexcept override {
        ++calls;
        source.assign(evidence.source().data(), evidence.source().size());
        source_instance.assign(evidence.source_instance().data(),
                               evidence.source_instance().size());
        peer_identity.assign(evidence.peer_identity_evidence().data(),
                             evidence.peer_identity_evidence().size());
        session_proof.assign(evidence.session_proof().data(),
                             evidence.session_proof().size());
        return {
            dockstation::core::SourceSessionVerificationStatus::Verified,
            {evidence.source(), evidence.source_instance(), "test-registration-rev"}
        };
    }
};

class NotFoundCursorReader final : public dockstation::core::SourceCursorReader {
public:
    int calls{0};
    dockstation::core::SourceCursorReadResult read_source_cursor(
        std::string_view) noexcept override {
        ++calls;
        return {dockstation::core::SourceCursorReadStatus::NotFound, {}};
    }
};

class ConfirmingSessionRepository final
    : public dockstation::core::SourceSessionRepository {
public:
    int calls{0};
    dockstation::core::SourceSessionConfirmationResult confirm_source_session(
        const dockstation::core::SourceCursorReadResult&,
        const dockstation::core::VerifiedSourceSession& verified) noexcept override {
        ++calls;
        return {
            dockstation::core::SourceSessionConfirmationStatus::Confirmed,
            {verified.source, verified.source_instance, std::nullopt}
        };
    }
};

class RecordingEventRepository final : public dockstation::core::EventRepository {
public:
    int calls{0};
    std::string last_event_id{};

    dockstation::core::EventRepositoryLookupResult find_by_id(
        std::string_view event_id) noexcept override {
        ++calls;
        last_event_id.assign(event_id.data(), event_id.size());
        return {dockstation::core::EventRepositoryLookupStatus::NotFound, {}};
    }
};

class RecordingEventProcessor final : public dockstation::core::EventProcessorPort {
public:
    int calls{0};
    dockstation::core::EventProcessorStatus process(
        const dockstation::core::Event&,
        dockstation::core::EventCategory,
        const dockstation::core::SourceCursor&) noexcept override {
        ++calls;
        return dockstation::core::EventProcessorStatus::Processed;
    }
};

// Production projector maps only reports with explicit subsystem context.

}  // namespace

int main() {
    const std::string controller_link_log_path =
        "/tmp/dockstation_controller_link_" +
        std::to_string(static_cast<long long>(::getpid())) + ".jsonl";
    dockstation::platform::logging::LoggerConfig logger_config{};
    logger_config.minimum_level = dockstation::platform::logging::LogLevel::Info;
    logger_config.file_path = controller_link_log_path;
    logger_config.write_to_stderr = false;
    const auto logger_initialized =
        dockstation::platform::logging::initialize_logger(logger_config);
    expect(logger_initialized.status ==
               dockstation::platform::logging::LoggerStatus::Available,
           "initializes a private test log file for ControllerLink transition assertions");

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
    for (std::size_t attempt = 0U; attempt < 4U; ++attempt) {
        service.poll();
    }
    expect(service.status().state == ControllerLinkServiceState::Error,
           "polling a terminal configuration error does not change ControllerLink state");

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
    for (std::size_t attempt = 0U; attempt < 4U; ++attempt) {
        service.poll();
    }
    expect(service.status().state == ControllerLinkServiceState::Error,
           "repeated polling does not generate additional state changes for the same error");

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

    RecordingEventProcessor global_processor{};
    RecordingEventProcessor operation_processor{};
    RecordingEventProcessor timeout_processor{};
    dockstation::communications::ControllerSubsystemStatusEventProjector event_projector{};

#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
    const std::string database_path =
        "/tmp/dockstation_authenticated_ingress_" +
        std::to_string(static_cast<long long>(::getpid())) + ".sqlite3";
    (void)std::remove(database_path.c_str());
    dockstation::platform::persistence::SqliteDatabase database{};
    const auto database_opened = database.open(
        dockstation::platform::persistence::SqliteDatabaseConfig{database_path, 100});
    expect(static_cast<bool>(database_opened) && database_opened.schema_version == 3,
           "opens production SQLite schema before the authenticated ingress test");

    dockstation::platform::persistence::SqliteSourceSessionStore source_store{database};
    dockstation::platform::persistence::SqliteSubsystemStatusStore status_store{database};
    const std::string registered_peer_identity{
        reinterpret_cast<const char*>(credentials.identity.data()),
        credentials.identity.size()};
    expect(source_store.create_source_registration(
               "station-1", "sensor/esp32", "source-rev-1",
               registered_peer_identity).status ==
               dockstation::platform::persistence::SqliteSourceRegistrationWriteStatus::Created,
           "provisions the exact local PSK identity as an ACTIVE source registration");
    expect(status_store.create_subsystem_registration(
               "station-1", "weather.temperature", "sensor/esp32",
               "subsystem-rev-1").status ==
               dockstation::platform::persistence::SqliteSubsystemRegistrationWriteStatus::Created,
           "provisions the subsystem only under its matching ACTIVE source");

    dockstation::core::RegistrySourceSessionVerifier source_verifier{
        source_store, "station-1"};
    dockstation::core::SubsystemStatusEventProcessor subsystem_status_processor{
        "station-1", event_projector, status_store, status_store};
    dockstation::core::StationEventPipeline event_pipeline{
        status_store, global_processor, operation_processor, timeout_processor,
        source_verifier, source_store, source_store, &subsystem_status_processor};
#else
    RecordingEventRepository event_repository{};
    CaptureVerifier source_verifier{};
    NotFoundCursorReader cursor_reader{};
    ConfirmingSessionRepository session_repository{};
    dockstation::core::StationEventPipeline event_pipeline{
        event_repository, global_processor, operation_processor, timeout_processor,
        source_verifier, cursor_reader, session_repository};
#endif

    dockstation::communications::AuthenticatedEventTransportAdapter event_adapter{
        e2e_service, event_pipeline, event_projector, "sensor/esp32",
        dockstation::core::EventValidationPolicy{true, true, false}};
    expect(event_adapter.is_configured(),
           "attaches the trusted event adapter before the ControllerLink service starts");

    ControllerLinkServiceTestAccess::set_credential_loader(
        e2e_service, &load_test_credentials);
    const ControllerLinkServiceConfig config{
        pty.slave, "repka-01", "esp32-01"};
    const auto started = e2e_service.start(config);
    expect(started.state == ControllerLinkServiceState::TlsHandshake,
           "starts the production service over the PTY without pre-claiming TLS");

    std::array<std::uint8_t, kControllerSessionProofSize> premature_proof{};
    premature_proof.fill(0xA5U);
    expect(!e2e_service.export_authenticated_session_proof(premature_proof) &&
               std::all_of(premature_proof.begin(), premature_proof.end(),
                           [](std::uint8_t value) noexcept { return value == 0U; }),
           "does not expose exporter proof before TLS and logical authentication complete");

    expect(complete_tls_handshake(e2e_service, peer, pty.master) &&
               e2e_service.status().tls_authenticated,
           "authenticates the complete service-to-peer TLS 1.3 PSK-DHE handshake");
    expect(e2e_service.authenticated_pair_identity().empty() &&
               event_adapter.statistics().authenticated_messages_received == 0U,
           "does not expose pair identity or dispatch messages before HELLO_ACK");

    std::array<std::uint8_t, kControllerCodecMaximumFrameSize> peer_frame{};
    ControllerCodecDecodeStorage peer_storage{};
    ControllerCodecDecodedMessage peer_message{};
    expect(read_service_message(e2e_service, peer, pty.master, peer_frame,
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
    expect(read_service_message(e2e_service, peer, pty.master, peer_frame,
                                peer_storage, peer_message) &&
               peer_message.envelope.type == ControllerMessageType::RequestSnapshot &&
               e2e_service.status().protocol_established,
           "establishes the logical session and emits the initial REQUEST_SNAPSHOT");

    std::this_thread::sleep_for(std::chrono::milliseconds{2100U});
    peer_storage = {};
    peer_message = {};
    const bool periodic_snapshot_received = read_service_message(
        e2e_service, peer, pty.master, peer_frame, peer_storage, peer_message);
    if (!periodic_snapshot_received ||
        peer_message.envelope.type != ControllerMessageType::RequestSnapshot ||
        !e2e_service.status().protocol_established) {
        const auto diagnostic_status = e2e_service.status();
        std::cerr << "DIAGNOSTIC periodic snapshot: received="
                  << periodic_snapshot_received
                  << " message_type="
                  << static_cast<unsigned int>(peer_message.envelope.type)
                  << " state=" << static_cast<unsigned int>(diagnostic_status.state)
                  << " tls=" << diagnostic_status.tls_authenticated
                  << " protocol=" << diagnostic_status.protocol_established
                  << " snapshots=" << diagnostic_status.telemetry_snapshots_received
                  << " errors=" << diagnostic_status.error_messages_received
                  << " rejected=" << diagnostic_status.rejected_frames
                  << " error_code="
                  << (diagnostic_status.error_code != nullptr
                          ? diagnostic_status.error_code : "(null)")
                  << " ssl_state=" << SSL_state_string_long(peer.ssl)
                  << " ssl_pending=" << SSL_pending(peer.ssl)
                  << " openssl_error=";
        const unsigned long openssl_error = ERR_peek_last_error();
        if (openssl_error != 0UL) {
            std::array<char, 256U> error_text{};
            ERR_error_string_n(openssl_error, error_text.data(), error_text.size());
            std::cerr << error_text.data();
        } else {
            std::cerr << "(none)";
        }
        std::cerr << '\n';
    }
    expect(periodic_snapshot_received &&
               peer_message.envelope.type == ControllerMessageType::RequestSnapshot &&
               e2e_service.status().protocol_established,
           "emits another REQUEST_SNAPSHOT after the two-second polling interval");

    const std::string established_pair_identity =
        std::string{e2e_service.authenticated_pair_identity()};
    const std::string expected_local_identity{
        reinterpret_cast<const char*>(credentials.identity.data()),
        credentials.identity.size()};
    expect(established_pair_identity == expected_local_identity &&
               event_adapter.statistics().authenticated_messages_received == 0U,
           "exposes the local pair identity only after HELLO_ACK, without dispatching handshake messages");

    std::array<std::uint8_t, kControllerSessionProofSize> session_proof{};
    expect(e2e_service.export_authenticated_session_proof(session_proof),
           "exposes the TLS exporter proof only after the authenticated logical session is established");
    const bool proof_nonzero = std::any_of(
        session_proof.begin(), session_proof.end(),
        [](std::uint8_t value) noexcept { return value != 0U; });
    expect(proof_nonzero, "the established session proof contains derived keying material");
    OPENSSL_cleanse(session_proof.data(), session_proof.size());

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

    expect(event_adapter.statistics().authenticated_messages_received == 1U &&
               event_adapter.statistics().projected_events == 0U &&
               event_adapter.statistics().pipeline_invocations == 0U &&
               event_adapter.statistics().unmapped_messages == 1U &&
               event_adapter.statistics().last_status ==
                   dockstation::communications::AuthenticatedEventTransportStatus::ProjectionNotApplicable,
           "accepted telemetry updates the separate telemetry snapshot but bypasses the SSM event pipeline");

    const ControllerErrorPayload subsystem_error{
        "DHT22_READ_FAILED", ControllerErrorSeverity::Warning,
        "DHT22 read failed", "I2C timeout",
        std::string_view{"weather.temperature"},
        ControllerSubsystemStatus::Degraded};
    const auto subsystem_error_envelope =
        peer_envelope(ControllerMessageType::Error, 3U);
    expect(send_peer_message(peer, pty.master, subsystem_error_envelope,
                             ControllerMessagePayload{subsystem_error}),
           "sends an ERROR with an explicit subsystem ID and normalized status");
    for (std::size_t attempt = 0U;
         attempt < 64U && event_adapter.statistics().pipeline_invocations == 0U;
         ++attempt) {
        e2e_service.poll();
    }

    const std::string expected_pair_identity{
        reinterpret_cast<const char*>(credentials.identity.data()),
        credentials.identity.size()};
    std::string expected_event_id = "controller-event:v1:";
    constexpr char hex_digits[] = "0123456789abcdef";
    for (const std::uint8_t byte : credentials.identity) {
        expected_event_id.push_back(hex_digits[(byte >> 4U) & 0x0FU]);
        expected_event_id.push_back(hex_digits[byte & 0x0FU]);
    }
    expected_event_id += ":12:sensor/esp32:13:esp32-boot-01:3";

    expect(event_adapter.statistics().authenticated_messages_received == 2U &&
               event_adapter.statistics().projected_events == 1U &&
               event_adapter.statistics().pipeline_invocations == 1U &&
               event_adapter.statistics().unmapped_messages == 1U &&
               event_adapter.statistics().last_status ==
                   dockstation::communications::AuthenticatedEventTransportStatus::PipelineInvoked &&
               event_adapter.statistics().last_session_status ==
                   dockstation::core::SourceSessionCoordinatorStatus::Confirmed
#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
               && event_adapter.statistics().last_event_status ==
                   dockstation::core::EventHandlingStatus::NoStateChange,
           "authenticated subsystem report passes the production F13 processor and commits to SQLite");

    const auto stored_event = status_store.find_by_id(expected_event_id);
    sqlite3_int64 history_count = 0;
    std::string current_event_id;
    expect(stored_event.status ==
               dockstation::core::EventRepositoryLookupStatus::Found &&
               stored_event.event.source == "sensor/esp32" &&
               stored_event.event.source_instance ==
                   std::optional<std::string_view>{"esp32-boot-01"} &&
               stored_event.event.source_sequence == std::optional<std::uint64_t>{3U} &&
               stored_event.event.event_type == "SUBSYSTEM_STATUS_UPDATED" &&
               stored_event.event.payload.has_value() &&
               stored_event.event.payload->find("\"status\":\"DEGRADED\"") !=
                   std::string_view::npos &&
               query_integer(database.native_handle(),
                   "SELECT count(*) FROM subsystem_status_history;", history_count) &&
               history_count == 1 &&
               query_text(database.native_handle(),
                   "SELECT last_event_id FROM subsystem_status_current "
                   "WHERE station_id='station-1' AND subsystem_id='weather.temperature';",
                   current_event_id) &&
               current_event_id == expected_event_id,
           "SQLite stores the original Event, typed history and current pointer together");
    const auto cursor = source_store.read_source_cursor("sensor/esp32");
    expect(cursor.status == dockstation::core::SourceCursorReadStatus::Found &&
               cursor.cursor.source_instance ==
                   std::optional<std::string_view>{"esp32-boot-01"} &&
               cursor.cursor.last_sequence == std::optional<std::uint64_t>{3U},
           "source cursor advances only with the committed subsystem report");
#else
               && event_adapter.statistics().last_event_status ==
                   dockstation::core::EventHandlingStatus::Unsupported,
           "without production persistence the subsystem route remains fail-closed");
    expect(source_verifier.calls == 1 && cursor_reader.calls == 1 &&
               session_repository.calls == 1 && event_repository.calls == 1 &&
               event_repository.last_event_id == expected_event_id,
           "one projected status event is bound to the accepted source sequence");
#endif
#if !defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
    expect(source_verifier.source == "sensor/esp32" &&
               source_verifier.source_instance == "esp32-boot-01" &&
               source_verifier.peer_identity == expected_pair_identity,
           "Event and evidence use the configured source, accepted peer instance and local TLS PSK identity");
#endif
    std::array<std::uint8_t, kControllerSessionProofSize> adapter_bound_proof{};
    expect(e2e_service.export_authenticated_session_proof(adapter_bound_proof) &&
               std::any_of(adapter_bound_proof.begin(), adapter_bound_proof.end(),
                   [](std::uint8_t value) noexcept { return value != 0U; }),
           "exporter proof remains available only while the authenticated TLS session is live");
    OPENSSL_cleanse(adapter_bound_proof.data(), adapter_bound_proof.size());

    // A repeated peer error code in the same authenticated session is accepted
    // as a report but must not produce another journal entry.
    const ControllerErrorPayload repeated_subsystem_error{
        "DHT22_READ_FAILED", ControllerErrorSeverity::Warning,
        "DHT22 read failed again", "I2C timeout", {}, {}};
    const auto repeated_error_envelope =
        peer_envelope(ControllerMessageType::Error, 4U);
    expect(send_peer_message(peer, pty.master, repeated_error_envelope,
                             ControllerMessagePayload{repeated_subsystem_error}),
           "sends a repeated authenticated controller error code");
    for (std::size_t attempt = 0U;
         attempt < 64U && e2e_service.status().error_messages_received < 2U;
         ++attempt) {
        e2e_service.poll();
    }
    expect(e2e_service.status().state == ControllerLinkServiceState::Established &&
               e2e_service.status().error_messages_received == 2U,
           "accepts the repeated controller report without losing the authenticated link");
#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
    expect(stored_event.event.event_id == expected_event_id,
           "Event ID is bound to pair identity, source, authenticated instance and sequence");
#else
    expect(event_repository.last_event_id == expected_event_id,
           "Event ID is bound to pair identity, source, authenticated instance and sequence");
#endif

#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
    const ControllerErrorPayload recovered_subsystem{
        "STATE_RESTORED", ControllerErrorSeverity::Info,
        "Temperature sensor responding again", {},
        std::string_view{"weather.temperature"},
        ControllerSubsystemStatus::Pass};
    const auto recovered_envelope =
        peer_envelope(ControllerMessageType::Error, 5U);
    expect(send_peer_message(peer, pty.master, recovered_envelope,
                             ControllerMessagePayload{recovered_subsystem}),
           "sends a newer authenticated PASS observation for the same subsystem");
    for (std::size_t attempt = 0U;
         attempt < 64U && event_adapter.statistics().pipeline_invocations < 2U;
         ++attempt) {
        e2e_service.poll();
    }
    std::string second_event_id = expected_event_id;
    second_event_id.replace(second_event_id.size() - 1U, 1U, "5");
    const auto second_event = status_store.find_by_id(second_event_id);
    sqlite3_int64 second_history_count = 0;
    std::string second_current_event_id;
    expect(event_adapter.statistics().pipeline_invocations == 2U &&
               event_adapter.statistics().last_session_status ==
                   dockstation::core::SourceSessionCoordinatorStatus::AlreadyCurrent &&
               event_adapter.statistics().last_event_status ==
                   dockstation::core::EventHandlingStatus::NoStateChange &&
               second_event.status ==
                   dockstation::core::EventRepositoryLookupStatus::Found &&
               second_event.event.payload.has_value() &&
               second_event.event.payload->find("\"status\":\"PASS\"") !=
                   std::string_view::npos &&
               query_integer(database.native_handle(),
                   "SELECT count(*) FROM subsystem_status_history;", second_history_count) &&
               second_history_count == 2 &&
               query_text(database.native_handle(),
                   "SELECT last_event_id FROM subsystem_status_current "
                   "WHERE station_id='station-1' AND subsystem_id='weather.temperature';",
                   second_current_event_id) &&
               second_current_event_id == second_event_id,
           "newer PASS appends history and updates the current pointer without changing StationState");
    const auto advanced_cursor = source_store.read_source_cursor("sensor/esp32");
    expect(advanced_cursor.status == dockstation::core::SourceCursorReadStatus::Found &&
               advanced_cursor.cursor.last_sequence == std::optional<std::uint64_t>{5U},
           "second status commit advances the source cursor from sequence 3 to 5");
#endif

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

    // Stop sending peer traffic long enough to exercise the service-level
    // heartbeat deadline. Recovery must create a new TLS transport and repeat
    // HELLO/HELLO_ACK rather than reusing the stale authenticated stream.
    std::this_thread::sleep_for(std::chrono::milliseconds{5100U});
    e2e_service.poll();
    expect(e2e_service.status().state == ControllerLinkServiceState::LinkLost &&
               !e2e_service.status().tls_authenticated &&
               !e2e_service.status().protocol_established &&
               e2e_service.authenticated_pair_identity().empty(),
           "heartbeat timeout closes the stale TLS session and clears authenticated state");

    std::this_thread::sleep_for(std::chrono::milliseconds{2100U});
    e2e_service.poll();
    expect(e2e_service.status().state == ControllerLinkServiceState::TlsHandshake &&
               !e2e_service.status().tls_authenticated,
           "heartbeat timeout schedules a fresh UART/TLS connection attempt");

    // The previous peer process is gone with the timed-out TLS session.
    // Release its BIO pair before modelling a fresh ESP32-side TLS endpoint.
    peer.reset();
    TlsPeer recovery_peer;
    expect(recovery_peer.initialize(credentials),
           "initializes a new TLS peer for the reconnect acceptance path");
    const bool reconnect_tls_completed =
        complete_tls_handshake(e2e_service, recovery_peer, pty.master);
    if (!reconnect_tls_completed || !e2e_service.status().tls_authenticated) {
        const auto diagnostic_status = e2e_service.status();
        std::cerr << "DIAGNOSTIC reconnect TLS: completed="
                  << reconnect_tls_completed
                  << " state=" << static_cast<unsigned int>(diagnostic_status.state)
                  << " tls=" << diagnostic_status.tls_authenticated
                  << " protocol=" << diagnostic_status.protocol_established
                  << " error_code="
                  << (diagnostic_status.error_code != nullptr
                          ? diagnostic_status.error_code : "(null)")
                  << " ssl_state=" << SSL_state_string_long(recovery_peer.ssl)
                  << " ssl_error=" << SSL_get_error(recovery_peer.ssl, -1)
                  << " openssl_error=";
        const unsigned long openssl_error = ERR_peek_last_error();
        if (openssl_error != 0UL) {
            std::array<char, 256U> error_text{};
            ERR_error_string_n(openssl_error, error_text.data(), error_text.size());
            std::cerr << error_text.data();
        } else {
            std::cerr << "(none)";
        }
        std::cerr << '\n';
    }
    expect(reconnect_tls_completed && e2e_service.status().tls_authenticated,
           "reconnect completes a new TLS 1.3 PSK-DHE handshake");
    peer_storage = {};
    peer_message = {};
    expect(read_service_message(e2e_service, recovery_peer, pty.master,
                                peer_frame, peer_storage, peer_message) &&
               peer_message.envelope.type == ControllerMessageType::Hello,
           "reconnect starts a fresh ControllerLink HELLO exchange");
    const auto reconnect_hello_ack =
        peer_envelope(ControllerMessageType::HelloAck, 6U);
    expect(send_peer_message(
               recovery_peer, pty.master, reconnect_hello_ack,
               ControllerMessagePayload{EmptyControllerMessagePayload{}}),
           "the peer acknowledges the new logical session with its next sequence");
    peer_storage = {};
    peer_message = {};
    expect(read_service_message(e2e_service, recovery_peer, pty.master,
                                peer_frame, peer_storage, peer_message) &&
               peer_message.envelope.type == ControllerMessageType::RequestSnapshot &&
               e2e_service.status().state == ControllerLinkServiceState::Established,
           "reconnect reaches ESTABLISHED and requests a fresh telemetry snapshot");

    // A duplicate peer sequence is rejected by ControllerLinkSession before
    // replacing the last known good telemetry snapshot.
    expect(send_peer_message(recovery_peer, pty.master, telemetry_envelope,
                             ControllerMessagePayload{telemetry_payload}),
           "sends a deliberately duplicated telemetry sequence as a negative fixture");
    e2e_service.poll();
    expect(e2e_service.status().state == ControllerLinkServiceState::LinkLost &&
               e2e_service.status().rejected_frames == 1U &&
               e2e_service.telemetry_snapshot().available() &&
               e2e_service.telemetry_snapshot().sequence_number() == 2U,
           "rejects the replayed sequence, closes the link and retains the last accepted snapshot");
#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
    expect(event_adapter.statistics().authenticated_messages_received == 4U &&
               event_adapter.statistics().pipeline_invocations == 2U,
           "a replayed sequence never reaches the authenticated event adapter or SSM pipeline");
#else
    expect(event_adapter.statistics().authenticated_messages_received == 3U &&
               event_adapter.statistics().pipeline_invocations == 1U,
           "a replayed sequence never reaches the authenticated event adapter or SSM pipeline");
#endif
    expect(e2e_service.authenticated_pair_identity().empty(),
           "pair identity is no longer exposed after the TLS/ControllerLink session is lost");

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

#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
    expect(database.close(), "closes the SQLite ingress test database cleanly");
    (void)std::remove(database_path.c_str());
    (void)std::remove((database_path + "-journal").c_str());
    (void)std::remove((database_path + "-wal").c_str());
    (void)std::remove((database_path + "-shm").c_str());
#endif

    const std::string psk_bytes{
        reinterpret_cast<const char*>(credentials.psk.data()),
        credentials.psk.size()};
    const std::string identity_bytes{
        reinterpret_cast<const char*>(credentials.identity.data()),
        credentials.identity.size()};
    test_credentials = nullptr;
    credentials.clear();

    const auto logger_shutdown = dockstation::platform::logging::shutdown_logger();
    expect(logger_shutdown.status ==
               dockstation::platform::logging::LoggerStatus::Available,
           "flushes and closes the ControllerLink test journal");
    std::ifstream log_input(controller_link_log_path);
    const std::string log_contents{
        std::istreambuf_iterator<char>{log_input},
        std::istreambuf_iterator<char>{}};
    const auto count_occurrences = [&log_contents](std::string_view needle) {
        std::size_t count = 0U;
        std::size_t offset = 0U;
        while ((offset = log_contents.find(needle, offset)) != std::string::npos) {
            ++count;
            offset += needle.size();
        }
        return count;
    };
    expect(log_input.is_open() && !log_input.bad(),
           "opens and reads the ControllerLink journal file");
    expect(log_contents.find("ControllerLink state transition") != std::string::npos,
           "journal records ControllerLink state transitions");
    expect(log_contents.find("DHT22_READ_FAILED") != std::string::npos,
           "journal records authenticated controller error codes");
    expect(log_contents.find("\"severity\":\"WARNING\"") != std::string::npos,
           "journal records controller error severity");
    expect(log_contents.find("\"subsystem_id\":\"weather.temperature\"") != std::string::npos,
           "journal records the affected controller subsystem");
    expect(log_contents.find("CONTROLLER_MESSAGE_REJECTED") != std::string::npos,
           "journal records rejected controller protocol messages");
    expect(log_contents.find("CONTROLLER_HEARTBEAT_TIMEOUT") != std::string::npos,
           "journal records heartbeat timeout before a fresh reconnect attempt");
    expect(count_occurrences("CONTROLLER_CONFIGURATION_INVALID") == 2U,
           "repeated polls do not spam duplicate configuration-error records");
    expect(count_occurrences("DHT22_READ_FAILED") == 1U,
           "an authenticated controller error is recorded with its structured code");
    expect(log_contents.find("DHT22 read failed") == std::string::npos &&
               log_contents.find("I2C timeout") == std::string::npos,
           "free-form controller error descriptions are excluded from the shared journal");
    expect(log_contents.find(psk_bytes) == std::string::npos &&
               log_contents.find(identity_bytes) == std::string::npos,
           "ephemeral PSK and identity bytes never appear in the ControllerLink journal");
    (void)std::remove(controller_link_log_path.c_str());

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
