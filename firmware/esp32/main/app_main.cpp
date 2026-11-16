#include "esp_log.h"
#include "controller_psk_nvs_credential_store.hpp"

namespace {
constexpr char kTag[] = "DockStationESP32";
}

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "DockStation ESP32 firmware bootstrap started");
    ESP_LOGW(
        kTag,
        "Diagnostic-only build: UART, GPIO, credentials and sensors are disabled "
        "until the exact board profile is confirmed");
}


// Link-only acceptance hook. The ESP-IDF main component forces this symbol
// into the final ELF so the linker must resolve every dependency of the NVS
// provider, while app_main deliberately never calls it. This keeps the
// unprovisioned board from touching NVS or loading credentials at boot.
extern "C" void dockstation_link_check_psk_nvs_credential_store(void) {
    dockstation::firmware::esp32::ControllerPskNvsCredentialStore store;
    dockstation::firmware::esp32::ControllerMbedTlsPskCredentials credentials;
    (void)store.initialize();
    (void)store.load(credentials);
}
