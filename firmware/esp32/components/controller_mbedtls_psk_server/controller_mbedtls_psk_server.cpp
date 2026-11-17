#include "controller_mbedtls_psk_server.hpp"
#include <algorithm>
#include <cstdint>

#if defined(ESP_PLATFORM)
#include "bootloader_random.h"
#endif
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/version.h>

#if MBEDTLS_VERSION_NUMBER < 0x03060000
#error "DockStation ESP32 ControllerLink requires Mbed TLS 3.6.0 or newer"
#endif
#if !defined(MBEDTLS_SSL_PROTO_TLS1_3) || !defined(MBEDTLS_SSL_SRV_C) || \
    !defined(MBEDTLS_SSL_KEYING_MATERIAL_EXPORT) || \
    !defined(MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED)
#error "Enable TLS 1.3 server, PSK-Ephemeral and keying material export in Mbed TLS"
#endif

namespace dockstation::firmware::esp32 {
namespace {
constexpr char kDrbgPersonalization[] = "DockStation ControllerLink ESP32 TLS server";
constexpr int kTls13Ciphersuites[] = {MBEDTLS_TLS1_3_AES_128_GCM_SHA256, 0};
constexpr std::uint16_t kTls13Groups[] = {MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1, 0U};

[[nodiscard]] bool contains_nonzero(const std::uint8_t* bytes, std::size_t size) noexcept {
    std::uint8_t combined = 0U;
    for (std::size_t i = 0U; i < size; ++i) combined = static_cast<std::uint8_t>(combined | bytes[i]);
    return combined != 0U;
}
}  // namespace

void ControllerMbedTlsPskCredentials::clear() noexcept {
    mbedtls_platform_zeroize(psk.data(), psk.size());
    mbedtls_platform_zeroize(identity.data(), identity.size());
}
ControllerMbedTlsPskCredentials::~ControllerMbedTlsPskCredentials() { clear(); }
ControllerMbedTlsPskServer::~ControllerMbedTlsPskServer() { close(); }

bool ControllerMbedTlsPskServer::initialize(
    const ControllerMbedTlsPskCredentials& credentials, void* transport_context,
    mbedtls_ssl_send_t* send_callback, mbedtls_ssl_recv_t* receive_callback) noexcept {
    close();
    if (!contains_nonzero(credentials.psk.data(), credentials.psk.size()) ||
        !contains_nonzero(credentials.identity.data(), credentials.identity.size()) ||
        transport_context == nullptr || send_callback == nullptr || receive_callback == nullptr) {
        state_ = ControllerMbedTlsPskServerState::Failed;
        last_mbedtls_error_ = MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        return false;
    }

    std::copy(credentials.psk.begin(), credentials.psk.end(), psk_.begin());
    std::copy(credentials.identity.begin(), credentials.identity.end(), identity_.begin());
    credentials_ready_ = true;

    mbedtls_entropy_init(&entropy_);
    entropy_initialized_ = true;
    mbedtls_ctr_drbg_init(&ctr_drbg_);
    ctr_drbg_initialized_ = true;
    mbedtls_ssl_config_init(&config_);
    config_initialized_ = true;
    mbedtls_ssl_init(&ssl_);
    ssl_initialized_ = true;

    // ESP32 provides true hardware entropy only while RF is active or
    // the SAR-ADC entropy source is enabled. Seed CTR-DRBG while that source
    // is temporarily enabled, then disable it before ADC/I2S/RF can be used.
    // Call initialize() during the early controller startup phase, with no
    // concurrent ADC/RF users.
#if defined(ESP_PLATFORM)
    bootloader_random_enable();
#endif
    const int seed_result = mbedtls_ctr_drbg_seed(
        &ctr_drbg_, mbedtls_entropy_func, &entropy_,
        reinterpret_cast<const unsigned char*>(kDrbgPersonalization),
        sizeof(kDrbgPersonalization) - 1U);
#if defined(ESP_PLATFORM)
    bootloader_random_disable();
#endif
    if (seed_result != 0) {
        mark_failed(seed_result);
        return false;
    }

    int result = mbedtls_ssl_config_defaults(
        &config_, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) { mark_failed(result); return false; }

    configure_tls();
    result = mbedtls_ssl_setup(&ssl_, &config_);
    if (result != 0) { mark_failed(result); return false; }

    mbedtls_ssl_set_bio(&ssl_, transport_context, send_callback, receive_callback, nullptr);
    state_ = ControllerMbedTlsPskServerState::AwaitingHandshake;
    last_mbedtls_error_ = 0;
    return true;
}

ControllerMbedTlsHandshakeProgress ControllerMbedTlsPskServer::advance_handshake() noexcept {
    if (state_ == ControllerMbedTlsPskServerState::Established)
        return ControllerMbedTlsHandshakeProgress::Complete;
    if (state_ != ControllerMbedTlsPskServerState::AwaitingHandshake)
        return state_ == ControllerMbedTlsPskServerState::Failed
            ? ControllerMbedTlsHandshakeProgress::Failed
            : ControllerMbedTlsHandshakeProgress::NotConfigured;

    const int result = mbedtls_ssl_handshake(&ssl_);
    if (result == 0) {
        if (!pair_identity_accepted_ || !verify_negotiated_profile()) {
            mark_failed(MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE);
            return ControllerMbedTlsHandshakeProgress::Failed;
        }
        state_ = ControllerMbedTlsPskServerState::Established;
        return ControllerMbedTlsHandshakeProgress::Complete;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ) return ControllerMbedTlsHandshakeProgress::NeedRead;
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) return ControllerMbedTlsHandshakeProgress::NeedWrite;
    mark_failed(result);
    return ControllerMbedTlsHandshakeProgress::Failed;
}

ControllerMbedTlsIoResult ControllerMbedTlsPskServer::write_application_data(
    const std::uint8_t* bytes, std::size_t byte_count) noexcept {
    if (state_ != ControllerMbedTlsPskServerState::Established)
        return {state_ == ControllerMbedTlsPskServerState::Failed
                    ? ControllerMbedTlsIoStatus::Failed
                    : ControllerMbedTlsIoStatus::HandshakeIncomplete, 0U};
    if (byte_count == 0U) return {ControllerMbedTlsIoStatus::Success, 0U};
    if (bytes == nullptr) return {ControllerMbedTlsIoStatus::InvalidArgument, 0U};

    const int result = mbedtls_ssl_write(&ssl_, bytes, byte_count);
    if (result > 0) return {ControllerMbedTlsIoStatus::Success, static_cast<std::size_t>(result)};
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
        return {ControllerMbedTlsIoStatus::WouldBlock, 0U};
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || result == 0) {
        close();
        return {ControllerMbedTlsIoStatus::Closed, 0U};
    }
    mark_failed(result);
    return {ControllerMbedTlsIoStatus::Failed, 0U};
}

ControllerMbedTlsIoResult ControllerMbedTlsPskServer::read_application_data(
    std::uint8_t* destination, std::size_t destination_capacity) noexcept {
    if (state_ != ControllerMbedTlsPskServerState::Established)
        return {state_ == ControllerMbedTlsPskServerState::Failed
                    ? ControllerMbedTlsIoStatus::Failed
                    : ControllerMbedTlsIoStatus::HandshakeIncomplete, 0U};
    if (destination_capacity == 0U) return {ControllerMbedTlsIoStatus::Success, 0U};
    if (destination == nullptr) return {ControllerMbedTlsIoStatus::InvalidArgument, 0U};

    const int result = mbedtls_ssl_read(&ssl_, destination, destination_capacity);
    if (result > 0) return {ControllerMbedTlsIoStatus::Success, static_cast<std::size_t>(result)};
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
        return {ControllerMbedTlsIoStatus::WouldBlock, 0U};
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || result == 0) {
        close();
        return {ControllerMbedTlsIoStatus::Closed, 0U};
    }
    mark_failed(result);
    return {ControllerMbedTlsIoStatus::Failed, 0U};
}

bool ControllerMbedTlsPskServer::export_session_proof(
    std::array<std::uint8_t, hardware::interfaces::kControllerLinkSessionProofSize>& output) noexcept {
    mbedtls_platform_zeroize(output.data(), output.size());
    if (state_ != ControllerMbedTlsPskServerState::Established) return false;

    const int result = mbedtls_ssl_export_keying_material(
        &ssl_, output.data(), output.size(),
        hardware::interfaces::kControllerLinkSessionProofExporterLabel,
        sizeof(hardware::interfaces::kControllerLinkSessionProofExporterLabel) - 1U,
        reinterpret_cast<const unsigned char*>(hardware::interfaces::kControllerLinkSessionProofContext),
        sizeof(hardware::interfaces::kControllerLinkSessionProofContext) - 1U, 1);
    if (result != 0) {
        mbedtls_platform_zeroize(output.data(), output.size());
        return false;
    }
    return true;
}

void ControllerMbedTlsPskServer::close() noexcept {
    release_resources();
    state_ = ControllerMbedTlsPskServerState::Closed;
    last_mbedtls_error_ = 0;
}

int ControllerMbedTlsPskServer::select_pair_psk(
    void* context, mbedtls_ssl_context* ssl,
    const unsigned char* identity, std::size_t identity_size) noexcept {
    if (context == nullptr || ssl == nullptr || identity == nullptr)
        return MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY;
    auto* self = static_cast<ControllerMbedTlsPskServer*>(context);
    if (!self->credentials_ready_ || identity_size != self->identity_.size())
        return MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY;

    std::uint8_t difference = 0U;
    for (std::size_t i = 0U; i < self->identity_.size(); ++i)
        difference = static_cast<std::uint8_t>(difference | (identity[i] ^ self->identity_[i]));
    if (difference != 0U) return MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY;

    const int result = mbedtls_ssl_set_hs_psk(ssl, self->psk_.data(), self->psk_.size());
    if (result != 0) return result;
    self->pair_identity_accepted_ = true;
    return 0;
}

void ControllerMbedTlsPskServer::configure_tls() noexcept {
    mbedtls_ssl_conf_rng(&config_, mbedtls_ctr_drbg_random, &ctr_drbg_);
    mbedtls_ssl_conf_min_tls_version(&config_, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&config_, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_ciphersuites(&config_, kTls13Ciphersuites);
    // Only one group is enabled, so P-256 is the sole possible negotiation.
    mbedtls_ssl_conf_groups(&config_, kTls13Groups);
    mbedtls_ssl_conf_tls13_key_exchange_modes(
        &config_, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL);
    mbedtls_ssl_conf_psk_cb(&config_, &ControllerMbedTlsPskServer::select_pair_psk, this);
#if defined(MBEDTLS_SSL_EARLY_DATA)
    mbedtls_ssl_conf_early_data(&config_, MBEDTLS_SSL_EARLY_DATA_DISABLED);
#endif
#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_SRV_C) && \
    defined(MBEDTLS_SSL_PROTO_TLS1_3)
    mbedtls_ssl_conf_new_session_tickets(&config_, 0U);
#endif
}

bool ControllerMbedTlsPskServer::verify_negotiated_profile() const noexcept {
    return mbedtls_ssl_get_version_number(&ssl_) == MBEDTLS_SSL_VERSION_TLS1_3 &&
           mbedtls_ssl_get_ciphersuite_id_from_ssl(&ssl_) ==
               MBEDTLS_TLS1_3_AES_128_GCM_SHA256 &&
           pair_identity_accepted_;
}

void ControllerMbedTlsPskServer::mark_failed(int error) noexcept {
    last_mbedtls_error_ = error;
    state_ = ControllerMbedTlsPskServerState::Failed;
    release_resources();
}

void ControllerMbedTlsPskServer::release_resources() noexcept {
    if (ssl_initialized_) { mbedtls_ssl_free(&ssl_); ssl_initialized_ = false; }
    if (config_initialized_) { mbedtls_ssl_config_free(&config_); config_initialized_ = false; }
    if (ctr_drbg_initialized_) { mbedtls_ctr_drbg_free(&ctr_drbg_); ctr_drbg_initialized_ = false; }
    if (entropy_initialized_) { mbedtls_entropy_free(&entropy_); entropy_initialized_ = false; }
    mbedtls_platform_zeroize(psk_.data(), psk_.size());
    mbedtls_platform_zeroize(identity_.data(), identity_.size());
    credentials_ready_ = false;
    pair_identity_accepted_ = false;
}
}  // namespace dockstation::firmware::esp32
