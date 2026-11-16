#include "controller_psk_nvs_credential_store.hpp"

#include <cstddef>
#include <cstdint>

#include "esp_flash_encrypt.h"
#include "esp_partition.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace dockstation::firmware::esp32 {
namespace {

constexpr char kNvsPartitionLabel[] = "nvs";
constexpr char kNvsKeysPartitionLabel[] = "nvs_keys";
constexpr char kCredentialNamespace[] = "dockstation";
constexpr char kPskBlobKey[] = "pair_psk";
constexpr char kIdentityBlobKey[] = "pair_identity";
constexpr char kControllerIdKey[] = "controller_id";
constexpr char kPeerControllerIdKey[] = "peer_ctrl_id";
constexpr std::size_t kNvsMaximumKeyLength = 15U;
static_assert(sizeof(kCredentialNamespace) - 1U <= kNvsMaximumKeyLength);
static_assert(sizeof(kPskBlobKey) - 1U <= kNvsMaximumKeyLength);
static_assert(sizeof(kIdentityBlobKey) - 1U <= kNvsMaximumKeyLength);
static_assert(sizeof(kControllerIdKey) - 1U <= kNvsMaximumKeyLength);
static_assert(sizeof(kPeerControllerIdKey) - 1U <= kNvsMaximumKeyLength);
constexpr std::size_t kMinimumNvsKeysPartitionSize =
    (2U * static_cast<std::size_t>(NVS_KEY_SIZE)) + 16U;

[[nodiscard]] bool contains_nonzero(
    const std::uint8_t* bytes, std::size_t size) noexcept {
    std::uint8_t combined = 0U;
    for (std::size_t i = 0U; i < size; ++i) {
        combined = static_cast<std::uint8_t>(combined | bytes[i]);
    }
    return combined != 0U;
}

}  // namespace

bool ControllerPskNvsCredentialStore::initialize() noexcept {
    if (initialized_) {
        last_error_ = ESP_OK;
        return true;
    }

    // Never initialize plaintext NVS and never ask ESP-IDF to generate keys
    // here. This provider is intentionally fail-closed on an unprovisioned unit.
    if (!esp_flash_encryption_enabled()) {
        last_error_ = ESP_ERR_NOT_SUPPORTED;
        return false;
    }

    const esp_partition_t* nvs_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_NVS,
        kNvsPartitionLabel);
    const esp_partition_t* keys_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS,
        kNvsKeysPartitionLabel);

    if (nvs_partition == nullptr || keys_partition == nullptr) {
        last_error_ = ESP_ERR_NOT_FOUND;
        return false;
    }

    // NVS data is encrypted by NVS itself. Its key partition must additionally
    // be covered by hardware Flash Encryption so the XTS keys are not exposed.
    if (nvs_partition->encrypted || !keys_partition->encrypted ||
        keys_partition->size < kMinimumNvsKeysPartitionSize) {
        last_error_ = ESP_ERR_INVALID_STATE;
        return false;
    }

    nvs_sec_cfg_t security_config{};
    esp_err_t result = nvs_flash_read_security_cfg(
        keys_partition, &security_config);
    if (result != ESP_OK) {
        mbedtls_platform_zeroize(&security_config, sizeof(security_config));
        last_error_ = result;
        return false;
    }

    // Explicit secure init uses existing keys only. No call to
    // nvs_flash_generate_keys(), nvs_flash_generate_keys_v2(), or nvs_flash_init().
    result = nvs_flash_secure_init_partition(
        kNvsPartitionLabel, &security_config);
    mbedtls_platform_zeroize(&security_config, sizeof(security_config));

    if (result != ESP_OK) {
        last_error_ = result;
        return false;
    }

    initialized_ = true;
    last_error_ = ESP_OK;
    return true;
}

bool ControllerPskNvsCredentialStore::load(
    ControllerMbedTlsPskCredentials& output) noexcept {
    output.clear();
    if (!initialized_) {
        last_error_ = ESP_ERR_INVALID_STATE;
        return false;
    }

    nvs_handle_t handle{0U};
    esp_err_t result = nvs_open_from_partition(
        kNvsPartitionLabel,
        kCredentialNamespace,
        NVS_READONLY,
        &handle);
    if (result != ESP_OK) {
        last_error_ = result;
        return false;
    }

    std::size_t psk_size = output.psk.size();
    result = nvs_get_blob(handle, kPskBlobKey, output.psk.data(), &psk_size);
    if (result == ESP_OK && psk_size != output.psk.size()) {
        result = ESP_ERR_INVALID_SIZE;
    }

    if (result == ESP_OK) {
        std::size_t identity_size = output.identity.size();
        result = nvs_get_blob(
            handle, kIdentityBlobKey, output.identity.data(), &identity_size);
        if (result == ESP_OK && identity_size != output.identity.size()) {
            result = ESP_ERR_INVALID_SIZE;
        }
    }

    nvs_close(handle);

    if (result == ESP_OK &&
        (!contains_nonzero(output.psk.data(), output.psk.size()) ||
         !contains_nonzero(output.identity.data(), output.identity.size()))) {
        result = ESP_ERR_INVALID_RESPONSE;
    }

    if (result != ESP_OK) {
        output.clear();
        last_error_ = result;
        return false;
    }

    last_error_ = ESP_OK;
    return true;
}

bool ControllerPskNvsCredentialStore::load_controller_ids(
    hardware::interfaces::ControllerStableIdPair& output) noexcept {
    output.clear();
    if (!initialized_) {
        last_error_ = ESP_ERR_INVALID_STATE;
        return false;
    }

    nvs_handle_t handle{0U};
    esp_err_t result = nvs_open_from_partition(
        kNvsPartitionLabel,
        kCredentialNamespace,
        NVS_READONLY,
        &handle);
    if (result != ESP_OK) {
        last_error_ = result;
        return false;
    }

    // nvs_get_str reports string storage size including the terminating NUL.
    std::size_t local_storage_size = output.local.size();
    result = nvs_get_str(
        handle, kControllerIdKey, output.local.data(), &local_storage_size);
    if (result == ESP_OK) {
        if (local_storage_size == 0U ||
            local_storage_size > output.local.size()) {
            result = ESP_ERR_INVALID_SIZE;
        } else {
            output.local_size = local_storage_size - 1U;
        }
    }

    if (result == ESP_OK) {
        std::size_t peer_storage_size = output.peer.size();
        result = nvs_get_str(
            handle, kPeerControllerIdKey, output.peer.data(),
            &peer_storage_size);
        if (result == ESP_OK) {
            if (peer_storage_size == 0U ||
                peer_storage_size > output.peer.size()) {
                result = ESP_ERR_INVALID_SIZE;
            } else {
                output.peer_size = peer_storage_size - 1U;
            }
        }
    }

    nvs_close(handle);

    if (result == ESP_OK && !output.is_valid()) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    if (result != ESP_OK) {
        output.clear();
        last_error_ = result;
        return false;
    }

    last_error_ = ESP_OK;
    return true;
}

}  // namespace dockstation::firmware::esp32
