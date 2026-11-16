#include "esp_log.h"

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
