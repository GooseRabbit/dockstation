#include "controller_instance_id_provider.hpp"

#include "bootloader_random.h"
#include "esp_random.h"

namespace dockstation::firmware::esp32 {

bool ControllerInstanceIdProvider::generate_for_current_boot() noexcept {
    if (generated_) {
        return false;
    }

    hardware::interfaces::ControllerInstanceIdEntropyBytes random_bytes{};

    // ESP-IDF normally disables the SAR-ADC entropy source before app_main.
    // This component is called before any ADC/I2S/RF use, so it can safely
    // enable the source temporarily and disable it immediately after filling.
    bootloader_random_enable();
    esp_fill_random(random_bytes.data(), random_bytes.size());
    bootloader_random_disable();

    hardware::interfaces::format_controller_instance_id_uuid_v4(
        random_bytes, value_);
    generated_ = true;
    return true;
}

std::string_view ControllerInstanceIdProvider::value() const noexcept {
    if (!generated_) {
        return {};
    }
    return {value_.data(), value_.size()};
}

}  // namespace dockstation::firmware::esp32
