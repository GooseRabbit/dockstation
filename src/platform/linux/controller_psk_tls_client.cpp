#include "platform/linux/controller_psk_tls_client.hpp"

#include <algorithm>
#include <cstring>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/crypto.h>

namespace dockstation::platform::linux_platform {
namespace {

constexpr unsigned char kTlsAes128GcmSha256[] = {0x13U, 0x01U};
constexpr const char* kTlsCipherSuite = "TLS_AES_128_GCM_SHA256";
constexpr const char* kTlsNamedGroup = "P-256";

[[nodiscard]] bool is_retryable_bio_result(BIO* bio) noexcept {
    return bio != nullptr && BIO_should_retry(bio) != 0;
}

}  // namespace

ControllerPskTlsClient::~ControllerPskTlsClient() {
    close();
}

bool ControllerPskTlsClient::initialize(
    const ControllerPskCredentials& credentials) noexcept {
    close();
    ERR_clear_error();
    const bool psk_is_nonzero = std::any_of(
        credentials.psk.begin(), credentials.psk.end(),
        [](std::uint8_t value) noexcept { return value != 0U; });
    const bool identity_is_nonzero = std::any_of(
        credentials.identity.begin(), credentials.identity.end(),
        [](std::uint8_t value) noexcept { return value != 0U; });
    if (!psk_is_nonzero || !identity_is_nonzero) {
        failed_ = true;
        return false;
    }
    credentials_ = credentials;
    credentials_ready_ = true;
    failed_ = false;
    last_openssl_error_ = 0UL;

    if (!configure_context()) {
        mark_failed();
        return false;
    }

    ssl_ = SSL_new(context_);
    if (ssl_ == nullptr) {
        mark_failed();
        return false;
    }
    SSL_set_app_data(ssl_, this);

    BIO* ssl_bio = nullptr;
    if (BIO_new_bio_pair(&ssl_bio, 0U, &transport_bio_, 0U) != 1 ||
        ssl_bio == nullptr || transport_bio_ == nullptr) {
        BIO_free(ssl_bio);
        mark_failed();
        return false;
    }

    // SSL takes ownership of ssl_bio. The opposite BIO remains owned here and
    // is the only interface exposed to the physical transport adapter.
    SSL_set_bio(ssl_, ssl_bio, ssl_bio);
    SSL_set_connect_state(ssl_);
    return true;
}

void ControllerPskTlsClient::close() noexcept {
    if (ssl_ != nullptr) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (transport_bio_ != nullptr) {
        BIO_free(transport_bio_);
        transport_bio_ = nullptr;
    }
    if (context_ != nullptr) {
        SSL_CTX_free(context_);
        context_ = nullptr;
    }
    OPENSSL_cleanse(credentials_.psk.data(), credentials_.psk.size());
    OPENSSL_cleanse(credentials_.identity.data(), credentials_.identity.size());
    credentials_ready_ = false;
    failed_ = false;
    last_openssl_error_ = 0UL;
}

bool ControllerPskTlsClient::is_configured() const noexcept {
    return context_ != nullptr && ssl_ != nullptr && transport_bio_ != nullptr &&
           credentials_ready_ && !failed_;
}

bool ControllerPskTlsClient::is_handshake_complete() const noexcept {
    return is_configured() && SSL_is_init_finished(ssl_) == 1;
}

bool ControllerPskTlsClient::configure_context() noexcept {
    context_ = SSL_CTX_new(TLS_client_method());
    if (context_ == nullptr) {
        return false;
    }

    if (SSL_CTX_set_min_proto_version(context_, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context_, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_ciphersuites(context_, kTlsCipherSuite) != 1 ||
        SSL_CTX_set1_groups_list(context_, kTlsNamedGroup) != 1 ||
        SSL_CTX_set_max_early_data(context_, 0U) != 1 ||
        SSL_CTX_set_num_tickets(context_, 0U) != 1) {
        return false;
    }

    SSL_CTX_set_options(context_, SSL_OP_NO_TICKET);
    // OpenSSL requires PSK-DHE by default. This option explicitly permits
    // PSK-only key exchange, so clear it to preserve the DEC-066 requirement.
    SSL_CTX_clear_options(context_, SSL_OP_ALLOW_NO_DHE_KEX);
    SSL_CTX_set_session_cache_mode(context_, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_psk_use_session_callback(context_, &ControllerPskTlsClient::use_psk_session);
    SSL_CTX_set_mode(context_, SSL_MODE_ENABLE_PARTIAL_WRITE |
                                  SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return true;
}

int ControllerPskTlsClient::use_psk_session(
    SSL* ssl,
    const EVP_MD* digest,
    const unsigned char** identity,
    std::size_t* identity_length,
    SSL_SESSION** session) noexcept {
    if (ssl == nullptr || identity == nullptr || identity_length == nullptr ||
        session == nullptr) {
        return 0;
    }

    auto* self = static_cast<ControllerPskTlsClient*>(SSL_get_app_data(ssl));
    if (self == nullptr || !self->credentials_ready_ || self->failed_) {
        return 0;
    }
    if (digest != nullptr && EVP_MD_get_type(digest) != NID_sha256) {
        return 0;
    }

    const SSL_CIPHER* cipher = SSL_CIPHER_find(ssl, kTlsAes128GcmSha256);
    if (cipher == nullptr) {
        return 0;
    }

    SSL_SESSION* psk_session = SSL_SESSION_new();
    if (psk_session == nullptr ||
        SSL_SESSION_set1_master_key(psk_session,
                                    self->credentials_.psk.data(),
                                    self->credentials_.psk.size()) != 1 ||
        SSL_SESSION_set_cipher(psk_session, cipher) != 1 ||
        SSL_SESSION_set_protocol_version(psk_session, TLS1_3_VERSION) != 1 ||
        SSL_SESSION_set_max_early_data(psk_session, 0U) != 1) {
        SSL_SESSION_free(psk_session);
        return 0;
    }

    *identity = self->credentials_.identity.data();
    *identity_length = self->credentials_.identity.size();
    // Ownership transfers to OpenSSL. A fresh SSL_SESSION is created for
    // every callback invocation to avoid refcount/lifetime ambiguity.
    *session = psk_session;
    return 1;
}

ControllerTlsProgress ControllerPskTlsClient::advance_handshake() noexcept {
    if (!is_configured()) {
        return failed_ ? ControllerTlsProgress::Failed
                       : ControllerTlsProgress::NotConfigured;
    }
    if (is_handshake_complete()) {
        return ControllerTlsProgress::Complete;
    }

    ERR_clear_error();
    const int result = SSL_connect(ssl_);
    if (result == 1) {
        if (!verify_negotiated_profile()) {
            mark_failed();
            return ControllerTlsProgress::Failed;
        }
        return ControllerTlsProgress::Complete;
    }

    const int ssl_error = SSL_get_error(ssl_, result);
    if (ssl_error == SSL_ERROR_WANT_READ) {
        return ControllerTlsProgress::NeedRead;
    }
    if (ssl_error == SSL_ERROR_WANT_WRITE) {
        return ControllerTlsProgress::NeedWrite;
    }

    mark_failed();
    return ControllerTlsProgress::Failed;
}

ControllerTlsIoResult ControllerPskTlsClient::feed_encrypted_bytes(
    const std::uint8_t* bytes, std::size_t byte_count) noexcept {
    if (!is_configured()) {
        return {failed_ ? ControllerTlsIoStatus::Failed
                        : ControllerTlsIoStatus::NotConfigured, 0U};
    }
    if (byte_count == 0U) {
        return {ControllerTlsIoStatus::Success, 0U};
    }
    if (bytes == nullptr) {
        return {ControllerTlsIoStatus::InvalidArgument, 0U};
    }

    std::size_t written = 0U;
    ERR_clear_error();
    if (BIO_write_ex(transport_bio_, bytes, byte_count, &written) == 1) {
        return {written == byte_count ? ControllerTlsIoStatus::Success
                                      : ControllerTlsIoStatus::WouldBlock,
                written};
    }
    if (is_retryable_bio_result(transport_bio_)) {
        return {ControllerTlsIoStatus::WouldBlock, written};
    }
    mark_failed();
    return {ControllerTlsIoStatus::Failed, written};
}

ControllerTlsIoResult ControllerPskTlsClient::take_encrypted_bytes(
    std::uint8_t* destination, std::size_t destination_capacity) noexcept {
    if (!is_configured()) {
        return {failed_ ? ControllerTlsIoStatus::Failed
                        : ControllerTlsIoStatus::NotConfigured, 0U};
    }
    if (destination_capacity == 0U) {
        return {ControllerTlsIoStatus::Success, 0U};
    }
    if (destination == nullptr) {
        return {ControllerTlsIoStatus::InvalidArgument, 0U};
    }

    std::size_t received = 0U;
    ERR_clear_error();
    if (BIO_read_ex(transport_bio_, destination, destination_capacity, &received) == 1) {
        return {ControllerTlsIoStatus::Success, received};
    }
    if (is_retryable_bio_result(transport_bio_)) {
        return {ControllerTlsIoStatus::WouldBlock, 0U};
    }
    if (BIO_eof(transport_bio_) == 1) {
        return {ControllerTlsIoStatus::Closed, 0U};
    }
    mark_failed();
    return {ControllerTlsIoStatus::Failed, 0U};
}

ControllerTlsIoResult ControllerPskTlsClient::write_application_data(
    const std::uint8_t* bytes, std::size_t byte_count) noexcept {
    if (!is_configured()) {
        return {failed_ ? ControllerTlsIoStatus::Failed
                        : ControllerTlsIoStatus::NotConfigured, 0U};
    }
    if (!is_handshake_complete()) {
        return {ControllerTlsIoStatus::HandshakeIncomplete, 0U};
    }
    if (byte_count == 0U) {
        return {ControllerTlsIoStatus::Success, 0U};
    }
    if (bytes == nullptr) {
        return {ControllerTlsIoStatus::InvalidArgument, 0U};
    }

    std::size_t written = 0U;
    ERR_clear_error();
    const int ssl_result = SSL_write_ex(ssl_, bytes, byte_count, &written);
    if (ssl_result == 1) {
        return {ControllerTlsIoStatus::Success, written};
    }
    const int ssl_error = SSL_get_error(ssl_, ssl_result);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        return {ControllerTlsIoStatus::WouldBlock, written};
    }
    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        return {ControllerTlsIoStatus::Closed, written};
    }
    mark_failed();
    return {ControllerTlsIoStatus::Failed, written};
}

ControllerTlsIoResult ControllerPskTlsClient::read_application_data(
    std::uint8_t* destination, std::size_t destination_capacity) noexcept {
    if (!is_configured()) {
        return {failed_ ? ControllerTlsIoStatus::Failed
                        : ControllerTlsIoStatus::NotConfigured, 0U};
    }
    if (!is_handshake_complete()) {
        return {ControllerTlsIoStatus::HandshakeIncomplete, 0U};
    }
    if (destination_capacity == 0U) {
        return {ControllerTlsIoStatus::Success, 0U};
    }
    if (destination == nullptr) {
        return {ControllerTlsIoStatus::InvalidArgument, 0U};
    }

    std::size_t received = 0U;
    ERR_clear_error();
    const int ssl_result = SSL_read_ex(ssl_, destination, destination_capacity, &received);
    if (ssl_result == 1) {
        return {ControllerTlsIoStatus::Success, received};
    }
    const int ssl_error = SSL_get_error(ssl_, ssl_result);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        return {ControllerTlsIoStatus::WouldBlock, received};
    }
    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        return {ControllerTlsIoStatus::Closed, received};
    }
    mark_failed();
    return {ControllerTlsIoStatus::Failed, received};
}

int ControllerPskTlsClient::negotiated_tls_version() const noexcept {
    return is_handshake_complete() ? SSL_version(ssl_) : 0;
}

const char* ControllerPskTlsClient::negotiated_cipher_name() const noexcept {
    if (!is_handshake_complete()) {
        return nullptr;
    }
    const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl_);
    return cipher == nullptr ? nullptr : SSL_CIPHER_get_name(cipher);
}

int ControllerPskTlsClient::negotiated_group_id() const noexcept {
    return is_handshake_complete() ? SSL_get_negotiated_group(ssl_) : 0;
}

bool ControllerPskTlsClient::verify_negotiated_profile() noexcept {
    if (SSL_version(ssl_) != TLS1_3_VERSION) {
        return false;
    }
    // OpenSSL reports a TLS 1.3 external-PSK handshake as a reused session too.
    // Tickets and the session cache are disabled above, so this flag confirms
    // that the configured PSK was selected rather than certificate fallback.
    if (SSL_session_reused(ssl_) != 1) {
        return false;
    }
    const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl_);
    if (cipher == nullptr ||
        std::strcmp(SSL_CIPHER_get_name(cipher), kTlsCipherSuite) != 0) {
        return false;
    }
    return SSL_get_negotiated_group(ssl_) == NID_X9_62_prime256v1;
}

void ControllerPskTlsClient::mark_failed() noexcept {
    failed_ = true;
    last_openssl_error_ = ERR_peek_last_error();
    if (last_openssl_error_ == 0UL) {
        last_openssl_error_ = ERR_get_error();
    }
    // Close immediately and erase pair credentials so no queued plaintext or
    // retained PSK can be reused after a fatal alert/authentication failure.
    // Reinitialization requires a fresh full TLS handshake and credentials.
    if (ssl_ != nullptr) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (transport_bio_ != nullptr) {
        BIO_free(transport_bio_);
        transport_bio_ = nullptr;
    }
    if (context_ != nullptr) {
        SSL_CTX_free(context_);
        context_ = nullptr;
    }
    OPENSSL_cleanse(credentials_.psk.data(), credentials_.psk.size());
    OPENSSL_cleanse(credentials_.identity.data(), credentials_.identity.size());
    credentials_ready_ = false;
}

}  // namespace dockstation::platform::linux_platform
