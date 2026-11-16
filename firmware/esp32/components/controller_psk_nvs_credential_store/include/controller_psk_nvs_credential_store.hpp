#pragma once

#include "controller_mbedtls_psk_server.hpp"
#include "esp_err.h"

namespace dockstation::firmware::esp32 {

// Read-only provider for pair PSK[32] + identity[16] stored in encrypted NVS.
// This class never provisions, generates, rotates or erases keys.
class ControllerPskNvsCredentialStore final {
public:
    ControllerPskNvsCredentialStore() noexcept = default;

    ControllerPskNvsCredentialStore(const ControllerPskNvsCredentialStore&) = delete;
    ControllerPskNvsCredentialStore& operator=(const ControllerPskNvsCredentialStore&) = delete;
    ControllerPskNvsCredentialStore(ControllerPskNvsCredentialStore&&) = delete;
    ControllerPskNvsCredentialStore& operator=(ControllerPskNvsCredentialStore&&) = delete;

    // Requires hardware Flash Encryption and an existing encrypted nvs_keys
    // partition. Initializes the "nvs" data partition with its existing NVS
    // encryption keys; does not create or repair encryption keys.
    [[nodiscard]] bool initialize() noexcept;

    // Loads exact-size blobs from a read-only NVS handle. Clears output on
    // any failure; secret bytes are never logged or returned as text.
    [[nodiscard]] bool load(ControllerMbedTlsPskCredentials& output) noexcept;

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
    [[nodiscard]] esp_err_t last_error() const noexcept { return last_error_; }

private:
    bool initialized_{false};
    esp_err_t last_error_{ESP_OK};
};

}  // namespace dockstation::firmware::esp32
