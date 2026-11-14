#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "platform/linux/controller_psk_tls_client.hpp"

// The in-memory BIO peer below tests the OpenSSL TLS state machine only. It is
// not a UART/GPIO/ESP32 emulator and does not claim Mbed TLS interoperability
// or physical transport verification.

using namespace dockstation::platform::linux_platform;

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

struct TestServerCredentials final {
    ControllerPskCredentials credentials{};
    bool allow_certificate_fallback{false};
};

int find_psk_session(
    SSL* ssl,
    const unsigned char* identity,
    std::size_t identity_length,
    SSL_SESSION** session) noexcept {
    if (ssl == nullptr || identity == nullptr || session == nullptr) {
        return 0;
    }
    auto* server = static_cast<TestServerCredentials*>(SSL_get_app_data(ssl));
    if (server == nullptr) {
        return 0;
    }
    if (identity_length != server->credentials.identity.size() ||
        CRYPTO_memcmp(identity, server->credentials.identity.data(), identity_length) != 0) {
        // OpenSSL's TLS 1.3 callback may continue with certificate
        // authentication when it returns success with no PSK session.
        // This test mode proves that the client refuses that downgrade path.
        *session = nullptr;
        return server->allow_certificate_fallback ? 1 : 0;
    }

    constexpr unsigned char suite_id[] = {0x13U, 0x01U};
    const SSL_CIPHER* cipher = SSL_CIPHER_find(ssl, suite_id);
    SSL_SESSION* psk_session = SSL_SESSION_new();
    if (cipher == nullptr || psk_session == nullptr ||
        SSL_SESSION_set1_master_key(psk_session,
                                    server->credentials.psk.data(),
                                    server->credentials.psk.size()) != 1 ||
        SSL_SESSION_set_cipher(psk_session, cipher) != 1 ||
        SSL_SESSION_set_protocol_version(psk_session, TLS1_3_VERSION) != 1 ||
        SSL_SESSION_set_max_early_data(psk_session, 0U) != 1) {
        SSL_SESSION_free(psk_session);
        return 0;
    }
    *session = psk_session;
    return 1;
}

bool make_credentials(ControllerPskCredentials& credentials) noexcept {
    return RAND_bytes(credentials.psk.data(),
                      static_cast<int>(credentials.psk.size())) == 1 &&
           RAND_bytes(credentials.identity.data(),
                      static_cast<int>(credentials.identity.size())) == 1;
}

struct TestServer final {
    SSL_CTX* context{nullptr};
    SSL* ssl{nullptr};
    BIO* transport_bio{nullptr};
    EVP_PKEY* fallback_key{nullptr};
    X509* fallback_certificate{nullptr};
    TestServerCredentials credentials{};

    ~TestServer() {
        if (ssl != nullptr) {
            SSL_free(ssl);
        }
        if (transport_bio != nullptr) {
            BIO_free(transport_bio);
        }
        if (context != nullptr) {
            SSL_CTX_free(context);
        }
        X509_free(fallback_certificate);
        EVP_PKEY_free(fallback_key);
        OPENSSL_cleanse(credentials.credentials.psk.data(),
                        credentials.credentials.psk.size());
        OPENSSL_cleanse(credentials.credentials.identity.data(),
                        credentials.credentials.identity.size());
    }
};

bool initialize_fallback_certificate(TestServer& server) noexcept {
    EVP_PKEY_CTX* key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (key_context == nullptr || EVP_PKEY_keygen_init(key_context) <= 0 ||
        EVP_PKEY_keygen(key_context, &server.fallback_key) <= 0) {
        EVP_PKEY_CTX_free(key_context);
        return false;
    }
    EVP_PKEY_CTX_free(key_context);

    server.fallback_certificate = X509_new();
    if (server.fallback_certificate == nullptr ||
        X509_set_version(server.fallback_certificate, 2L) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(server.fallback_certificate), 1L) != 1 ||
        X509_gmtime_adj(X509_getm_notBefore(server.fallback_certificate), 0L) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(server.fallback_certificate), 3600L) == nullptr ||
        X509_set_pubkey(server.fallback_certificate, server.fallback_key) != 1) {
        return false;
    }
    X509_NAME* subject = X509_get_subject_name(server.fallback_certificate);
    constexpr unsigned char common_name[] = "dec066-test-fallback";
    if (subject == nullptr ||
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                   common_name, -1, -1, 0) != 1 ||
        X509_set_issuer_name(server.fallback_certificate, subject) != 1 ||
        X509_sign(server.fallback_certificate, server.fallback_key, nullptr) <= 0) {
        return false;
    }
    return true;
}

bool initialize_server(TestServer& server,
                       const ControllerPskCredentials& credentials,
                       int minimum_version = TLS1_3_VERSION,
                       int maximum_version = TLS1_3_VERSION,
                       bool allow_certificate_fallback = false,
                       bool allow_psk_without_dhe = false) noexcept {
    server.credentials.credentials = credentials;
    server.credentials.allow_certificate_fallback = allow_certificate_fallback;
    server.context = SSL_CTX_new(TLS_server_method());
    if (server.context == nullptr ||
        SSL_CTX_set_min_proto_version(server.context, minimum_version) != 1 ||
        SSL_CTX_set_max_proto_version(server.context, maximum_version) != 1 ||
        SSL_CTX_set_ciphersuites(server.context, "TLS_AES_128_GCM_SHA256") != 1 ||
        SSL_CTX_set1_groups_list(server.context,
                                 allow_psk_without_dhe ? "P-384" : "P-256") != 1 ||
        SSL_CTX_set_max_early_data(server.context, 0U) != 1 ||
        SSL_CTX_set_num_tickets(server.context, 0U) != 1) {
        return false;
    }
    if (allow_certificate_fallback &&
        (!initialize_fallback_certificate(server) ||
         SSL_CTX_use_certificate(server.context, server.fallback_certificate) != 1 ||
         SSL_CTX_use_PrivateKey(server.context, server.fallback_key) != 1)) {
        return false;
    }
    SSL_CTX_set_options(server.context, SSL_OP_NO_TICKET);
    SSL_CTX_clear_options(server.context, SSL_OP_ALLOW_NO_DHE_KEX);
    if (allow_psk_without_dhe) {
        SSL_CTX_set_options(server.context, SSL_OP_ALLOW_NO_DHE_KEX);
    }
    SSL_CTX_set_session_cache_mode(server.context, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_psk_find_session_callback(server.context, &find_psk_session);

    server.ssl = SSL_new(server.context);
    if (server.ssl == nullptr) {
        return false;
    }
    SSL_set_app_data(server.ssl, &server.credentials);

    BIO* ssl_bio = nullptr;
    if (BIO_new_bio_pair(&ssl_bio, 0U, &server.transport_bio, 0U) != 1 ||
        ssl_bio == nullptr || server.transport_bio == nullptr) {
        BIO_free(ssl_bio);
        return false;
    }
    SSL_set_bio(server.ssl, ssl_bio, ssl_bio);
    SSL_set_accept_state(server.ssl);
    return true;
}

bool pump_client_to_server(ControllerPskTlsClient& client, BIO* server_transport) noexcept {
    // Deliberately move small chunks to verify that TLS accepts an arbitrary
    // byte stream rather than depending on UART packet/frame boundaries.
    std::array<std::uint8_t, 23U> bytes{};
    for (std::size_t attempt = 0U; attempt < 256U; ++attempt) {
        const auto result = client.take_encrypted_bytes(bytes.data(), bytes.size());
        if (result.status == ControllerTlsIoStatus::WouldBlock) {
            return true;
        }
        if (result.status != ControllerTlsIoStatus::Success) {
            return false;
        }
        if (result.bytes_transferred == 0U) {
            return true;
        }
        std::size_t written = 0U;
        if (BIO_write_ex(server_transport, bytes.data(), result.bytes_transferred,
                         &written) != 1 || written != result.bytes_transferred) {
            return false;
        }
    }
    return false;
}

bool pump_server_to_client(ControllerPskTlsClient& client, BIO* server_transport) noexcept {
    std::array<std::uint8_t, 19U> bytes{};
    for (std::size_t attempt = 0U; attempt < 256U; ++attempt) {
        std::size_t received = 0U;
        if (BIO_read_ex(server_transport, bytes.data(), bytes.size(), &received) != 1) {
            return BIO_should_retry(server_transport) != 0;
        }
        if (received == 0U) {
            return true;
        }
        const auto result = client.feed_encrypted_bytes(bytes.data(), received);
        if (result.status != ControllerTlsIoStatus::Success ||
            result.bytes_transferred != received) {
            return false;
        }
    }
    return false;
}

bool complete_handshake(ControllerPskTlsClient& client, TestServer& server) noexcept {
    bool server_complete = false;
    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
        const auto client_progress = client.advance_handshake();
        if (client_progress == ControllerTlsProgress::Failed ||
            client_progress == ControllerTlsProgress::NotConfigured) {
            return false;
        }

        if (!server_complete) {
            const int result = SSL_accept(server.ssl);
            if (result == 1) {
                server_complete = true;
            } else {
                const int error = SSL_get_error(server.ssl, result);
                if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                    return false;
                }
            }
        }

        if (!pump_client_to_server(client, server.transport_bio) ||
            !pump_server_to_client(client, server.transport_bio)) {
            return false;
        }
        if (server_complete && client.is_handshake_complete()) {
            return true;
        }
    }
    return false;
}

bool exchange_application_data(ControllerPskTlsClient& client, TestServer& server) noexcept {
    constexpr std::array<std::uint8_t, 7U> request{{'D', 'S', '-', 'P', 'I', 'N', 'G'}};
    constexpr std::array<std::uint8_t, 7U> response{{'D', 'S', '-', 'P', 'O', 'N', 'G'}};
    std::array<std::uint8_t, 32U> buffer{};
    std::size_t received = 0U;

    const auto client_write = client.write_application_data(request.data(), request.size());
    if (client_write.status != ControllerTlsIoStatus::Success ||
        client_write.bytes_transferred != request.size() ||
        !pump_client_to_server(client, server.transport_bio)) {
        return false;
    }
    if (SSL_read_ex(server.ssl, buffer.data(), buffer.size(), &received) != 1 ||
        received != request.size() ||
        CRYPTO_memcmp(buffer.data(), request.data(), request.size()) != 0) {
        return false;
    }

    std::size_t written = 0U;
    if (SSL_write_ex(server.ssl, response.data(), response.size(), &written) != 1 ||
        written != response.size() || !pump_server_to_client(client, server.transport_bio)) {
        return false;
    }
    const auto client_read = client.read_application_data(buffer.data(), buffer.size());
    return client_read.status == ControllerTlsIoStatus::Success &&
           client_read.bytes_transferred == response.size() &&
           CRYPTO_memcmp(buffer.data(), response.data(), response.size()) == 0;
}

bool tampered_record_is_rejected(
    const ControllerPskCredentials& credentials) noexcept {
    ControllerPskTlsClient client;
    TestServer server;
    if (!client.initialize(credentials) || !initialize_server(server, credentials) ||
        !complete_handshake(client, server)) {
        return false;
    }

    constexpr std::array<std::uint8_t, 6U> response{{'S', 'E', 'C', 'R', 'E', 'T'}};
    std::size_t written = 0U;
    if (SSL_write_ex(server.ssl, response.data(), response.size(), &written) != 1 ||
        written != response.size()) {
        return false;
    }

    std::array<std::uint8_t, 4096U> encrypted{};
    std::size_t encrypted_size = 0U;
    if (BIO_read_ex(server.transport_bio, encrypted.data(), encrypted.size(),
                    &encrypted_size) != 1 || encrypted_size < 6U) {
        return false;
    }
    encrypted[encrypted_size - 1U] ^= 0x01U;
    const auto fed = client.feed_encrypted_bytes(encrypted.data(), encrypted_size);
    if (fed.status != ControllerTlsIoStatus::Success ||
        fed.bytes_transferred != encrypted_size) {
        return false;
    }

    std::array<std::uint8_t, 32U> plaintext{};
    const auto read = client.read_application_data(plaintext.data(), plaintext.size());
    return read.status == ControllerTlsIoStatus::Failed && client.is_failed();
}

bool run_pair(const ControllerPskCredentials& client_credentials,
              const ControllerPskCredentials& server_credentials,
              bool exchange_data,
              int server_minimum = TLS1_3_VERSION,
              int server_maximum = TLS1_3_VERSION,
              bool allow_certificate_fallback = false,
              bool allow_psk_without_dhe = false) noexcept {
    ControllerPskTlsClient client;
    TestServer server;
    if (!client.initialize(client_credentials) ||
        !initialize_server(server, server_credentials, server_minimum, server_maximum,
                           allow_certificate_fallback, allow_psk_without_dhe)) {
        return false;
    }
    const bool handshake = complete_handshake(client, server);
    if (!handshake || !client.is_handshake_complete()) {
        return false;
    }
    if (client.negotiated_tls_version() != TLS1_3_VERSION ||
        client.negotiated_cipher_name() == nullptr ||
        std::strcmp(client.negotiated_cipher_name(), "TLS_AES_128_GCM_SHA256") != 0 ||
        client.negotiated_group_id() != NID_X9_62_prime256v1) {
        return false;
    }
    return !exchange_data || exchange_application_data(client, server);
}

}  // namespace

int main() {
    ControllerPskCredentials credentials{};
    expect(make_credentials(credentials), "generate ephemeral 256-bit PSK and opaque identity");

    ControllerPskCredentials empty_credentials{};
    ControllerPskTlsClient rejects_empty_credentials;
    expect(!rejects_empty_credentials.initialize(empty_credentials) &&
               rejects_empty_credentials.is_failed(),
           "rejects an all-zero PSK or identity instead of accepting placeholders");

    ControllerPskTlsClient unconnected_client;
    expect(unconnected_client.initialize(credentials), "initializes TLS 1.3 PSK client");
    constexpr std::array<std::uint8_t, 4U> early_plaintext{{'N', 'O', 'P', 'E'}};
    const auto early_write = unconnected_client.write_application_data(
        early_plaintext.data(), early_plaintext.size());
    expect(early_write.status == ControllerTlsIoStatus::HandshakeIncomplete &&
               early_write.bytes_transferred == 0U,
           "does not expose application plaintext before authenticated handshake");
    unconnected_client.close();

    expect(run_pair(credentials, credentials, true),
           "completes TLS 1.3 PSK-DHE handshake and exchanges encrypted application data");

    ControllerPskCredentials wrong_psk = credentials;
    expect(RAND_bytes(wrong_psk.psk.data(), static_cast<int>(wrong_psk.psk.size())) == 1,
           "generate a different ephemeral PSK for negative test");
    expect(!run_pair(credentials, wrong_psk, false),
           "rejects a peer that presents the wrong PSK");

    ControllerPskCredentials wrong_identity = credentials;
    expect(RAND_bytes(wrong_identity.identity.data(),
                      static_cast<int>(wrong_identity.identity.size())) == 1,
           "generate a different ephemeral PSK identity for negative test");
    expect(!run_pair(credentials, wrong_identity, false),
           "rejects an unknown PSK identity");

    expect(!run_pair(credentials, credentials, false,
                     TLS1_2_VERSION, TLS1_2_VERSION),
           "rejects a TLS 1.2-only peer without protocol downgrade");

    expect(!run_pair(credentials, wrong_identity, false,
                     TLS1_3_VERSION, TLS1_3_VERSION, true),
           "rejects certificate fallback when the peer does not accept the external PSK");

    expect(!run_pair(credentials, credentials, false,
                     TLS1_3_VERSION, TLS1_3_VERSION, false, true),
           "rejects a server that allows PSK-only key exchange without ephemeral DHE");

    expect(tampered_record_is_rejected(credentials),
           "rejects a modified TLS application record and closes the session");

    OPENSSL_cleanse(credentials.psk.data(), credentials.psk.size());
    OPENSSL_cleanse(credentials.identity.data(), credentials.identity.size());
    OPENSSL_cleanse(wrong_psk.psk.data(), wrong_psk.psk.size());
    OPENSSL_cleanse(wrong_psk.identity.data(), wrong_psk.identity.size());
    OPENSSL_cleanse(wrong_identity.psk.data(), wrong_identity.psk.size());
    OPENSSL_cleanse(wrong_identity.identity.data(), wrong_identity.identity.size());

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
