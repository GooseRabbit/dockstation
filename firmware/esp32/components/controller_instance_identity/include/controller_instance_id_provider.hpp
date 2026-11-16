#pragma once

#include <string_view>

#include "hardware/interfaces/controller_instance_identity.hpp"

namespace dockstation::firmware::esp32 {

// Owns the ephemeral controller_instance_id for exactly one controller boot.
// Call generate_for_current_boot() early, before ADC, I2S or RF initialization.
class ControllerInstanceIdProvider final {
public:
    ControllerInstanceIdProvider() noexcept = default;

    ControllerInstanceIdProvider(const ControllerInstanceIdProvider&) = delete;
    ControllerInstanceIdProvider& operator=(
        const ControllerInstanceIdProvider&) = delete;
    ControllerInstanceIdProvider(ControllerInstanceIdProvider&&) = delete;
    ControllerInstanceIdProvider& operator=(
        ControllerInstanceIdProvider&&) = delete;

    // Enables the ESP-IDF entropy source only for this short generation step.
    // A second call is rejected so this object keeps one ID for this boot.
    [[nodiscard]] bool generate_for_current_boot() noexcept;

    [[nodiscard]] bool is_generated() const noexcept { return generated_; }

    // Empty until generated; the returned view remains valid for this object's
    // lifetime and can be copied into ControllerLinkPeerSessionConfig.
    [[nodiscard]] std::string_view value() const noexcept;

private:
    hardware::interfaces::ControllerInstanceIdText value_{};
    bool generated_{false};
};

}  // namespace dockstation::firmware::esp32
