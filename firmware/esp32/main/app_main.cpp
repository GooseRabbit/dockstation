#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string_view>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "DHT.h"
#include "controller_psk_nvs_credential_store.hpp"
#include "controller_instance_id_provider.hpp"
#include "controller_link_firmware_runtime.hpp"
#include "controller_link_reconnect_backoff.hpp"
#include "controller_uart_transport.hpp"
#include "dht22_sampling_scheduler.hpp"
#include "dht22_telemetry_adapter.hpp"
#include "hardware/interfaces/controller_link_startup_readiness.hpp"
#include "hardware/interfaces/controller_stable_identity.hpp"

namespace {
constexpr char kTag[] = "DockStationESP32";

// ControllerLink remains default-off until the exact module/SoC,
// Flash/partition layout, UART pinout and electrical compatibility are verified.
// This guard does not disable the independently configured DHT22 GPIO inputs.
dockstation::firmware::esp32::ControllerLinkFirmwareRuntimeStartConfig
    g_controller_link_start_config{};

// Kept for the lifetime of this controller boot. The future runtime start
// receives this provider; the ID itself is not persisted or logged.
dockstation::firmware::esp32::ControllerInstanceIdProvider
    g_controller_instance_id{};

// Owns the stable IDs because the runtime configuration holds string_views
// into these buffers. They remain valid for the full application lifetime.
dockstation::hardware::interfaces::ControllerStableIdPair
    g_controller_stable_ids{};

// Credentials stay in static storage only until the runtime copies them into
// the TLS server. The polling task clears this buffer immediately afterward.
dockstation::firmware::esp32::ControllerPskNvsCredentialStore
    g_controller_credential_store{};
dockstation::firmware::esp32::ControllerMbedTlsPskCredentials
    g_controller_link_credentials{};
dockstation::firmware::esp32::ControllerLinkFirmwareRuntime
    g_controller_link_runtime{};

constexpr std::uint32_t kControllerLinkTaskStackSizeBytes = 12U * 1024U;
constexpr UBaseType_t kControllerLinkTaskPriority = tskIDLE_PRIORITY + 3U;

constexpr std::uint64_t kMemoryReportIntervalMs = 60000U;

// Temporary plaintext prototype telemetry channel.
// UART0 emits log-framed DS1 telemetry through the board's USB-UART bridge.
// UART2 remains a separate raw 115200 8N1 GPIO17/GPIO16 host connection.
constexpr uart_port_t kPrototypeUartPort = UART_NUM_2;
constexpr gpio_num_t kPrototypeUartTxPin = GPIO_NUM_17;
constexpr gpio_num_t kPrototypeUartRxPin = GPIO_NUM_16;
constexpr int kPrototypeUartBaudRate = 115200;
constexpr std::uint64_t kPrototypeTelemetryIntervalMs = 2000U;
bool g_prototype_uart_enabled{false};
std::uint64_t g_prototype_telemetry_sequence{0U};


// Sensor acquisition is independent from ControllerLink readiness. This flag
// is enabled only after the complete transport profile and credentials pass.
bool g_controller_link_enabled{false};

void log_runtime_memory(const char* phase) noexcept {
    const std::size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const std::size_t largest_block =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const std::size_t minimum_free_heap =
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // ESP-IDF's FreeRTOS port reports this high-water mark in bytes.
    const UBaseType_t stack_high_water_bytes =
        uxTaskGetStackHighWaterMark(nullptr);

    ESP_LOGI(
        kTag,
        "Memory[%s]: heap_free=%u B heap_largest_block=%u B heap_min_free=%u B task_stack_high_water=%u B",
        phase,
        static_cast<unsigned>(free_heap),
        static_cast<unsigned>(largest_block),
        static_cast<unsigned>(minimum_free_heap),
        static_cast<unsigned>(stack_high_water_bytes));
}

void log_startup_blockers(
    const dockstation::hardware::interfaces::ControllerLinkStartupReadiness&
        readiness) noexcept {
    using dockstation::hardware::interfaces::ControllerLinkStartupBlocker;

    if (readiness.is_blocked_by(
            ControllerLinkStartupBlocker::HardwareProfileNotConfirmed)) {
        ESP_LOGW(kTag, "ControllerLink blocked: exact ESP32 hardware profile is not confirmed");
    }
    if (readiness.is_blocked_by(
            ControllerLinkStartupBlocker::UartConfigurationInvalid)) {
        ESP_LOGW(kTag, "ControllerLink blocked: explicit UART port/TX/RX profile is missing or invalid");
    }
    if (readiness.is_blocked_by(
            ControllerLinkStartupBlocker::PairCredentialsUnavailable)) {
        ESP_LOGW(kTag, "ControllerLink blocked: encrypted pair credentials are unavailable");
    }
    if (readiness.is_blocked_by(
            ControllerLinkStartupBlocker::InstanceIdentityUnavailable)) {
        ESP_LOGW(kTag, "ControllerLink blocked: per-boot instance identity is unavailable");
    }
    if (readiness.is_blocked_by(
            ControllerLinkStartupBlocker::InstanceIdentityConfigurationMismatch)) {
        ESP_LOGW(kTag, "ControllerLink blocked: configured local instance ID conflicts with this boot");
    }
    if (readiness.is_blocked_by(
            ControllerLinkStartupBlocker::ProtocolVersionInvalid)) {
        ESP_LOGW(kTag, "ControllerLink blocked: protocol major version must be nonzero");
    }
    if (readiness.is_blocked_by(
            ControllerLinkStartupBlocker::LocalControllerIdMissing) ||
        readiness.is_blocked_by(
            ControllerLinkStartupBlocker::LocalControllerIdInvalid)) {
        ESP_LOGW(kTag, "ControllerLink blocked: stable local controller ID is missing or invalid");
    }
    if (readiness.is_blocked_by(
            ControllerLinkStartupBlocker::PeerControllerIdMissing) ||
        readiness.is_blocked_by(
            ControllerLinkStartupBlocker::PeerControllerIdInvalid)) {
        ESP_LOGW(kTag, "ControllerLink blocked: stable peer controller ID is missing or invalid");
    }
    if (readiness.is_blocked_by(
            ControllerLinkStartupBlocker::ControllerIdsNotDistinct)) {
        ESP_LOGW(kTag, "ControllerLink blocked: local and peer controller IDs must differ");
    }
}

[[nodiscard]] std::uint64_t monotonic_milliseconds() noexcept {
    const std::int64_t value = esp_timer_get_time();
    return value > 0 ? static_cast<std::uint64_t>(value) / 1000U : 0U;
}


// DHT sensor assignments. Select the model explicitly for each input:
// DHT11 matches the currently installed KY-015 prototype module.
struct DhtSensorChannel final {
    gpio_num_t pin{GPIO_NUM_NC};
    DHT* sensor{nullptr};
    std::uint8_t sensor_type{DHT11};
    dockstation::firmware::esp32::Dht22SamplingScheduler scheduler{};
    dockstation::firmware::esp32::DhtSensorReading reading{};
    bool has_reading{false};
    std::uint64_t measured_at_us{0U};
    char measured_at[32]{};
    bool has_logged_read_status{false};
    bool last_read_succeeded{false};
    std::uint64_t last_diagnostic_log_ms{0U};
};

constexpr gpio_num_t kDhtSensor1DataPin = GPIO_NUM_25;
constexpr gpio_num_t kDhtSensor2DataPin = GPIO_NUM_26;
constexpr std::uint8_t kDhtSensor1Type = DHT11;
constexpr std::uint8_t kDhtSensor2Type = DHT11;

DHT g_dht_sensor_1{static_cast<std::uint8_t>(kDhtSensor1DataPin), kDhtSensor1Type};
DHT g_dht_sensor_2{static_cast<std::uint8_t>(kDhtSensor2DataPin), kDhtSensor2Type};

std::array<DhtSensorChannel, 2U> g_dht_sensor_channels{};

bool initialize_prototype_uart() noexcept {
    uart_config_t uart_config{};
    uart_config.baud_rate = kPrototypeUartBaudRate;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.rx_flow_ctrl_thresh = 0U;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t result = uart_param_config(kPrototypeUartPort, &uart_config);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Prototype UART configuration failed: %s", esp_err_to_name(result));
        return false;
    }
    result = uart_set_pin(
        kPrototypeUartPort, kPrototypeUartTxPin, kPrototypeUartRxPin,
        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Prototype UART pin setup failed: %s", esp_err_to_name(result));
        return false;
    }
    result = uart_driver_install(kPrototypeUartPort, 1024, 256, 0, nullptr, 0);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Prototype UART driver install failed: %s", esp_err_to_name(result));
        return false;
    }

    ESP_LOGW(kTag,
        "PLAINTEXT PROTOTYPE UART active: UART2 TX=GPIO%d RX=GPIO%d baud=%d 8N1",
        static_cast<int>(kPrototypeUartTxPin),
        static_cast<int>(kPrototypeUartRxPin), kPrototypeUartBaudRate);
    return true;
}

void send_prototype_dht22_telemetry() noexcept {
    const std::uint64_t now_us = static_cast<std::uint64_t>(esp_timer_get_time());
    char values[4][24]{};
    for (std::size_t index = 0U; index < g_dht_sensor_channels.size(); ++index) {
        const auto& channel = g_dht_sensor_channels[index];
        const bool fresh = channel.has_reading &&
            now_us >= channel.measured_at_us &&
            now_us - channel.measured_at_us <= 5000000U;
        const std::size_t temperature_index = index * 2U;
        const std::size_t humidity_index = temperature_index + 1U;
        if (fresh) {
            const double temperature =
                static_cast<double>(channel.reading.temperature_tenths_c) / 10.0;
            const double humidity =
                static_cast<double>(channel.reading.relative_humidity_tenths_percent) / 10.0;
            (void)std::snprintf(values[temperature_index], sizeof(values[temperature_index]),
                                "%.1f", temperature);
            (void)std::snprintf(values[humidity_index], sizeof(values[humidity_index]),
                                "%.1f", humidity);
        } else {
            std::snprintf(values[temperature_index], sizeof(values[temperature_index]), "NA");
            std::snprintf(values[humidity_index], sizeof(values[humidity_index]), "NA");
        }
    }

    ++g_prototype_telemetry_sequence;
    if (g_prototype_telemetry_sequence == 0U) ++g_prototype_telemetry_sequence;
    char frame[160]{};
    const int length = std::snprintf(
        frame, sizeof(frame), "DS1,%llu,%llu,%s,%s,%s,%s\n",
        static_cast<unsigned long long>(g_prototype_telemetry_sequence),
        static_cast<unsigned long long>(monotonic_milliseconds()),
        values[0], values[1], values[2], values[3]);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(frame)) {
        ESP_LOGW(kTag, "Prototype telemetry frame formatting failed");
        return;
    }
    // ESP_LOGI writes one complete UART0 console line alongside normal logs.
    // Linux extracts the DS1 payload after the standard ESP-IDF "TAG: " prefix.
    ESP_LOGI(kTag, "%.*s", length - 1, frame);

    // Preserve the original raw UART2 path for a directly wired Linux host.
    if (!g_prototype_uart_enabled) return;
    const int written = uart_write_bytes(
        kPrototypeUartPort, frame, static_cast<size_t>(length));
    if (written != length) {
        ESP_LOGW(kTag, "Prototype UART2 telemetry write incomplete: %d/%d", written, length);
    }
}

void initialize_dht22_channels() noexcept {
    g_dht_sensor_channels[0].pin = kDhtSensor1DataPin;
    g_dht_sensor_channels[0].sensor = &g_dht_sensor_1;
    g_dht_sensor_channels[0].sensor_type = kDhtSensor1Type;
    g_dht_sensor_channels[1].pin = kDhtSensor2DataPin;
    g_dht_sensor_channels[1].sensor = &g_dht_sensor_2;
    g_dht_sensor_channels[1].sensor_type = kDhtSensor2Type;

    const std::uint64_t now_ms = monotonic_milliseconds();
    for (auto& channel : g_dht_sensor_channels) {
        if (channel.sensor == nullptr || channel.pin == GPIO_NUM_NC ||
            !GPIO_IS_VALID_GPIO(channel.pin)) continue;
        channel.sensor->begin();
        channel.scheduler.initialize(now_ms);
    }

    ESP_LOGI(kTag, "DHT11/DHT22 channels configured: sensor1=GPIO%d type=%s sensor2=GPIO%d type=%s",
        static_cast<int>(g_dht_sensor_channels[0].pin),
        g_dht_sensor_channels[0].sensor_type == DHT11 ? "DHT11" : "DHT22",
        static_cast<int>(g_dht_sensor_channels[1].pin),
        g_dht_sensor_channels[1].sensor_type == DHT11 ? "DHT11" : "DHT22");
}

void log_dht_sensor_read_status(
    std::size_t index, DhtSensorChannel& channel, bool success,
    std::uint64_t now_ms) noexcept {
    const bool changed = !channel.has_logged_read_status ||
        channel.last_read_succeeded != success;
    const bool repeated_failure_due = channel.has_logged_read_status &&
        !success && now_ms >= channel.last_diagnostic_log_ms &&
        now_ms - channel.last_diagnostic_log_ms >= 10000U;
    if (!changed && !repeated_failure_due) return;

    const char* const status = success ? "SUCCESS" : "READ_FAILED";
    if (success) {
        ESP_LOGI(kTag, "DHT sensor=%u gpio=%d read_status=%s",
            static_cast<unsigned>(index + 1U), static_cast<int>(channel.pin), status);
    } else {
        ESP_LOGW(kTag, "DHT sensor=%u gpio=%d read_status=%s",
            static_cast<unsigned>(index + 1U), static_cast<int>(channel.pin), status);
    }
    channel.has_logged_read_status = true;
    channel.last_read_succeeded = success;
    channel.last_diagnostic_log_ms = now_ms;
}

void poll_dht22_channels() noexcept {
    const std::uint64_t now_ms = monotonic_milliseconds();
    for (std::size_t index = 0U; index < g_dht_sensor_channels.size(); ++index) {
        auto& channel = g_dht_sensor_channels[index];
        if (channel.sensor == nullptr || channel.pin == GPIO_NUM_NC ||
            !GPIO_IS_VALID_GPIO(channel.pin)) continue;
        if (channel.scheduler.try_begin_read(now_ms).status !=
            dockstation::firmware::esp32::Dht22SamplingStatus::Ready) continue;

        const float temperature_c = channel.sensor->readTemperature();
        const float humidity_percent = channel.sensor->readHumidity();
        const bool is_dht11 = channel.sensor_type == DHT11;
        const float minimum_temperature_c = is_dht11 ? 0.0F : -40.0F;
        const float maximum_temperature_c = is_dht11 ? 50.0F : 80.0F;
        const bool success =
            std::isfinite(temperature_c) && std::isfinite(humidity_percent) &&
            temperature_c >= minimum_temperature_c &&
            temperature_c <= maximum_temperature_c &&
            humidity_percent >= 0.0F && humidity_percent <= 99.9F;
        log_dht_sensor_read_status(index, channel, success, now_ms);
        if (!success) continue;

        dockstation::firmware::esp32::DhtSensorReading reading{};
        reading.temperature_tenths_c = static_cast<std::int16_t>(
            std::lround(temperature_c * 10.0F));
        reading.relative_humidity_tenths_percent = static_cast<std::uint16_t>(
            std::lround(humidity_percent * 10.0F));
        channel.reading = reading;
        channel.has_reading = true;
        channel.measured_at_us = static_cast<std::uint64_t>(esp_timer_get_time());
        const int written = std::snprintf(
            channel.measured_at, sizeof(channel.measured_at), "mono_us_%llu",
            static_cast<unsigned long long>(channel.measured_at_us));
        if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(channel.measured_at)) {
            channel.has_reading = false;
            channel.measured_at[0] = '\0';
        }
    }
}
std::size_t build_dht_sensor_telemetry(
    std::uint64_t now_us,
    std::string_view request_timestamp,
    std::array<dockstation::hardware::interfaces::ControllerTelemetryMeasurement, 4U>& output) noexcept {
    using namespace dockstation::firmware::esp32;
    using dockstation::hardware::interfaces::ControllerSensorValue;
    using dockstation::hardware::interfaces::SensorQualityStatus;

    output = {};
    std::size_t count = 0U;
    for (std::size_t index = 0U; index < g_dht_sensor_channels.size(); ++index) {
        const auto& channel = g_dht_sensor_channels[index];
        const char* const sensor_prefix = index == 0U ? "dht22_1" : "dht22_2";
        const std::string_view measured_at =
            channel.has_reading ? std::string_view{channel.measured_at} : request_timestamp;

        if (channel.pin == GPIO_NUM_NC || !GPIO_IS_VALID_GPIO(channel.pin)) {
            output[count++] = {index == 0U ? "dht22_1.temperature" : "dht22_2.temperature",
                ControllerSensorValue{std::string_view{"NOT_CONFIGURED"}}, "status",
                SensorQualityStatus::NotConfigured, measured_at};
            output[count++] = {index == 0U ? "dht22_1.humidity" : "dht22_2.humidity",
                ControllerSensorValue{std::string_view{"NOT_CONFIGURED"}}, "status",
                SensorQualityStatus::NotConfigured, measured_at};
            continue;
        }

        if (!channel.has_reading) {
            output[count++] = {index == 0U ? "dht22_1.temperature" : "dht22_2.temperature",
                ControllerSensorValue{std::string_view{"UNAVAILABLE"}}, "status",
                SensorQualityStatus::Unavailable, measured_at};
            output[count++] = {index == 0U ? "dht22_1.humidity" : "dht22_2.humidity",
                ControllerSensorValue{std::string_view{"UNAVAILABLE"}}, "status",
                SensorQualityStatus::Unavailable, measured_at};
            continue;
        }

        std::array<dockstation::hardware::interfaces::ControllerTelemetryMeasurement, 2U> pair{};
        const auto quality = now_us >= channel.measured_at_us &&
            now_us - channel.measured_at_us > 5000000U
                ? SensorQualityStatus::Stale : SensorQualityStatus::Ok;
        const std::string_view temperature_id =
            index == 0U ? "dht22_1.temperature" : "dht22_2.temperature";
        const std::string_view humidity_id =
            index == 0U ? "dht22_1.humidity" : "dht22_2.humidity";
        if (make_dht22_telemetry_measurements(
                channel.reading, temperature_id, humidity_id,
                measured_at, quality, pair) ==
            Dht22TelemetryBuildStatus::Success) {
            output[count++] = pair[0];
            output[count++] = pair[1];
        }
        (void)sensor_prefix;
    }
    return count;
}

[[nodiscard]] bool start_or_restart_controller_link() noexcept {
    const auto peer_state = g_controller_link_runtime.status().peer.state;
    const bool started =
        peer_state == dockstation::firmware::esp32::
                          ControllerLinkPeerServiceState::NotConfigured
            ? g_controller_link_runtime.start(
                  g_controller_link_start_config,
                  g_controller_link_credentials,
                  g_controller_instance_id)
            : g_controller_link_runtime.restart_transport(
                  g_controller_link_credentials);
    g_controller_link_credentials.clear();
    return started;
}

void controller_link_polling_task(void*) {
    using dockstation::firmware::esp32::ControllerLinkReconnectBackoff;

    ControllerLinkReconnectBackoff reconnect_backoff{};
    initialize_dht22_channels();
    if (g_controller_link_enabled) {
        if (!start_or_restart_controller_link()) {
            const auto delay_ms =
                reconnect_backoff.schedule_after_failure(monotonic_milliseconds());
            const auto status = g_controller_link_runtime.status();
            log_startup_blockers(status.startup_readiness);
            ESP_LOGW(
                kTag,
                "ControllerLink startup failed; retry in %u ms (UART: %s, UART fault: %s, TLS: %d)",
                static_cast<unsigned>(delay_ms),
                esp_err_to_name(status.last_uart_error),
                dockstation::firmware::esp32::controller_uart_fault_name(status.last_uart_fault),
                status.peer.last_mbedtls_error);
        } else {
            ESP_LOGI(kTag, "ControllerLink runtime started; waiting for authenticated peer session");
        }
    } else {
        ESP_LOGI(kTag, "DHT22 sampling task active; ControllerLink remains NOT_CONFIGURED");
    }

    log_runtime_memory("task-start");
    std::uint64_t last_memory_report_ms = monotonic_milliseconds();
    std::uint64_t last_prototype_telemetry_ms = 0U;

    constexpr TickType_t requested_delay = pdMS_TO_TICKS(10U);
    constexpr TickType_t poll_delay =
        requested_delay == 0U ? 1U : requested_delay;

    for (;;) {
        const std::uint64_t now_ms = monotonic_milliseconds();
        poll_dht22_channels();

        if (last_prototype_telemetry_ms == 0U ||
             (now_ms >= last_prototype_telemetry_ms &&
              now_ms - last_prototype_telemetry_ms >= kPrototypeTelemetryIntervalMs)) {
            send_prototype_dht22_telemetry();
            last_prototype_telemetry_ms = now_ms;
        }

        // Keep the DHT22 acquisition lifecycle alive even when the physical
        // UART profile or protected ControllerLink credentials are not ready.
        // No NVS, UART or TLS operation is attempted in this mode.
        if (!g_controller_link_enabled) {
            if (now_ms >= last_memory_report_ms &&
                now_ms - last_memory_report_ms >= kMemoryReportIntervalMs) {
                log_runtime_memory("periodic");
                last_memory_report_ms = now_ms;
            }
            vTaskDelay(poll_delay);
            continue;
        }

        auto runtime_status = g_controller_link_runtime.status();

        if (runtime_status.state ==
            dockstation::firmware::esp32::
                ControllerLinkFirmwareRuntimeState::Running) {
            char sent_at[32]{};
            const int written = std::snprintf(
                sent_at, sizeof(sent_at), "mono_us_%lld",
                static_cast<long long>(esp_timer_get_time()));
            if (written <= 0 ||
                static_cast<std::size_t>(written) >= sizeof(sent_at)) {
                ESP_LOGE(kTag, "Could not format ControllerLink monotonic time");
                g_controller_link_runtime.stop();
                runtime_status = g_controller_link_runtime.status();
            } else {
                const std::string_view sent_at_view{
                    sent_at, static_cast<std::size_t>(written)};
                g_controller_link_runtime.poll(sent_at_view);

                if (g_controller_link_runtime.snapshot_request_pending()) {
                    // Publish logical station state. Unconfigured physical
                    // inputs are explicitly UNKNOWN; no values are fabricated.
                    std::array<dockstation::hardware::interfaces::ControllerTelemetryMeasurement, 4U>
                        sensor_measurements{};
                    const std::size_t sensor_count = build_dht_sensor_telemetry(
                        static_cast<std::uint64_t>(esp_timer_get_time()),
                        sent_at_view, sensor_measurements);
                    static_cast<void>(
                        g_controller_link_runtime.send_station_state_snapshot(
                            sent_at_view, sensor_measurements.data(), sensor_count));
                }
                runtime_status = g_controller_link_runtime.status();
            }

            if (runtime_status.peer.state ==
                dockstation::firmware::esp32::
                    ControllerLinkPeerServiceState::Established) {
                reconnect_backoff.reset();
            }

            if (runtime_status.state !=
                dockstation::firmware::esp32::
                    ControllerLinkFirmwareRuntimeState::Running) {
                const auto delay_ms =
                    reconnect_backoff.schedule_after_failure(now_ms);
                ESP_LOGW(
                    kTag,
                    "ControllerLink disconnected; recovery retry in %u ms (UART: %s, UART fault: %s, TLS: %d)",
                    static_cast<unsigned>(delay_ms),
                    esp_err_to_name(runtime_status.last_uart_error),
                    dockstation::firmware::esp32::controller_uart_fault_name(runtime_status.last_uart_fault),
                    runtime_status.peer.last_mbedtls_error);
            }
        } else if (reconnect_backoff.retry_due(now_ms)) {
            reconnect_backoff.begin_attempt();

            if (!g_controller_credential_store.load(
                    g_controller_link_credentials)) {
                g_controller_link_credentials.clear();
                const auto delay_ms =
                    reconnect_backoff.schedule_after_failure(now_ms);
                ESP_LOGW(
                    kTag,
                    "ControllerLink credentials could not be reloaded from protected NVS; retry in %u ms",
                    static_cast<unsigned>(delay_ms));
            } else if (!start_or_restart_controller_link()) {
                const auto delay_ms =
                    reconnect_backoff.schedule_after_failure(now_ms);
                const auto status = g_controller_link_runtime.status();
                ESP_LOGW(
                    kTag,
                    "ControllerLink reconnect attempt failed; retry in %u ms (UART: %s, UART fault: %s, TLS: %d)",
                    static_cast<unsigned>(delay_ms),
                    esp_err_to_name(status.last_uart_error),
                    dockstation::firmware::esp32::controller_uart_fault_name(status.last_uart_fault),
                    status.peer.last_mbedtls_error);
            } else {
                ESP_LOGI(kTag, "ControllerLink reconnect attempt started");
            }
        }

        if (now_ms >= last_memory_report_ms &&
            now_ms - last_memory_report_ms >= kMemoryReportIntervalMs) {
            log_runtime_memory("periodic");
            last_memory_report_ms = now_ms;
        }

        vTaskDelay(poll_delay);
    }
}
}  // namespace

extern "C" void app_main(void) {
    using dockstation::hardware::interfaces::ControllerLinkStartupReadinessInput;
    using dockstation::hardware::interfaces::
        evaluate_controller_link_startup_readiness;

    ESP_LOGI(kTag, "DockStation ESP32 firmware bootstrap started");
    initArduino();
    ESP_LOGI(kTag, "Arduino Core initialized for Adafruit DHT library");
    g_prototype_uart_enabled = initialize_prototype_uart();
    ESP_LOGI(kTag, "Prototype DHT22 telemetry over USB/UART0 is enabled");
    if (!g_prototype_uart_enabled) {
        ESP_LOGW(kTag, "Raw UART2 path unavailable; USB/UART0 telemetry remains enabled");
    }

    // DHT22 acquisition is useful independently of ControllerLink. Prepare
    // its task even when the physical UART profile is intentionally unset.
    g_controller_link_enabled = false;

    const bool controller_link_ready = []() noexcept {
        // Generate the per-boot identity before ADC, I2S or RF subsystems are used.
        if (!g_controller_instance_id.generate_for_current_boot()) {
            ESP_LOGE(kTag, "Could not generate controller instance identity");
            return false;
        }
        ESP_LOGI(kTag, "Per-boot controller instance identity generated");

        // The default hardware profile is deliberately unconfirmed. This check
        // happens before NVS initialization: an unknown board must not touch the
        // production credential store or attempt any transport setup.
        if (!g_controller_link_start_config.hardware_profile_confirmed) {
            ESP_LOGW(kTag, "ControllerLink remains NOT_CONFIGURED: physical hardware profile is not confirmed; secure NVS was not opened");
            return false;
        }

        // Require an explicit, non-console UART pin map before attempting any
        // credential access. The UART driver itself is still not installed here.
        if (!dockstation::firmware::esp32::ControllerUartTransport::is_valid_config(
                g_controller_link_start_config.uart)) {
            ESP_LOGW(kTag, "ControllerLink remains NOT_CONFIGURED: explicit UART port/TX/RX configuration is missing or invalid; secure NVS was not opened");
            return false;
        }

        // Read only pre-provisioned credentials from an existing encrypted NVS
        // partition. The store never generates, repairs, writes or rotates keys.
        auto& credential_store = g_controller_credential_store;
        if (!credential_store.initialize()) {
            ESP_LOGW(
                kTag,
                "ControllerLink remains NOT_CONFIGURED: protected NVS is unavailable (%s)",
                esp_err_to_name(credential_store.last_error()));
            return false;
        }

        if (!credential_store.load(g_controller_link_credentials)) {
            ESP_LOGW(
                kTag,
                "ControllerLink remains NOT_CONFIGURED: pre-provisioned PSK/identity are unavailable (%s)",
                esp_err_to_name(credential_store.last_error()));
            g_controller_link_credentials.clear();
            return false;
        }

        if (!credential_store.load_controller_ids(g_controller_stable_ids)) {
            ESP_LOGW(
                kTag,
                "ControllerLink remains NOT_CONFIGURED: stable local/peer controller IDs are unavailable or invalid (%s)",
                esp_err_to_name(credential_store.last_error()));
            g_controller_link_credentials.clear();
            g_controller_stable_ids.clear();
            return false;
        }

        // These string_views point into static-lifetime owned buffers above.
        // The stable IDs are public protocol identifiers, not credentials.
        g_controller_link_start_config.session.local_controller_id =
            g_controller_stable_ids.local_id();
        g_controller_link_start_config.session.peer_controller_id =
            g_controller_stable_ids.peer_id();

        const auto& session_config = g_controller_link_start_config.session;
        ControllerLinkStartupReadinessInput readiness_input{};
        readiness_input.hardware_profile_confirmed =
            g_controller_link_start_config.hardware_profile_confirmed;
        readiness_input.uart_configuration_valid =
            dockstation::firmware::esp32::ControllerUartTransport::is_valid_config(
                g_controller_link_start_config.uart);
        readiness_input.pair_credentials_available =
            true;  // Both fixed-size secret fields passed the read-only NVS loader.
        readiness_input.instance_identity_generated =
            g_controller_instance_id.is_generated();
        readiness_input.instance_identity_configuration_matches =
            g_controller_instance_id.is_generated() &&
            (session_config.local_controller_instance_id.empty() ||
             session_config.local_controller_instance_id ==
                 g_controller_instance_id.value());
        readiness_input.protocol_version_major =
            session_config.protocol_version.major;
        readiness_input.local_controller_id =
            g_controller_stable_ids.local_id();
        readiness_input.peer_controller_id =
            g_controller_stable_ids.peer_id();

        const auto readiness =
            evaluate_controller_link_startup_readiness(readiness_input);
        if (!readiness.is_ready()) {
            log_startup_blockers(readiness);
            g_controller_link_credentials.clear();
            return false;
        }
        return true;
    }();

    g_controller_link_enabled = controller_link_ready;
    if (!controller_link_ready) {
        ESP_LOGI(kTag, "ControllerLink is NOT_CONFIGURED; continuing with independent DHT22 subsystem");
    }

    // One task owns DHT sampling and, when configured, ControllerLink. This
    // avoids cross-task access to the latest sensor samples.
    const BaseType_t task_created = xTaskCreate(
        controller_link_polling_task,
        "dockstation_runtime",
        kControllerLinkTaskStackSizeBytes,
        nullptr,
        kControllerLinkTaskPriority,
        nullptr);
    if (task_created != pdPASS) {
        g_controller_link_credentials.clear();
        ESP_LOGE(kTag, "Could not create DockStation runtime polling task");
        return;
    }
    ESP_LOGI(kTag, "DockStation runtime polling task queued");
}

// Link-only acceptance hook. The main component forces this symbol into the
// final ELF so the linker resolves every dependency of the read-only NVS
// provider. It is never executed; app_main calls the same store API through
// its separately gated, normal startup path.
extern "C" void dockstation_link_check_psk_nvs_credential_store(void) {
    dockstation::firmware::esp32::ControllerPskNvsCredentialStore store;
    dockstation::firmware::esp32::ControllerMbedTlsPskCredentials credentials;
    (void)store.initialize();
    (void)store.load(credentials);
    dockstation::hardware::interfaces::ControllerStableIdPair controller_ids{};
    (void)store.load_controller_ids(controller_ids);
}

// Link-only acceptance hook for the runtime API. It is never called; its
// default-off configuration and ungenerated identity must reject before UART.
extern "C" void dockstation_link_check_controller_link_firmware_runtime(void) {
    dockstation::firmware::esp32::ControllerLinkFirmwareRuntime runtime;
    dockstation::firmware::esp32::ControllerLinkFirmwareRuntimeStartConfig
        startup_config{};
    dockstation::firmware::esp32::ControllerInstanceIdProvider ungenerated_identity{};
    dockstation::firmware::esp32::ControllerMbedTlsPskCredentials credentials{};
    (void)runtime.start(startup_config, credentials, ungenerated_identity);
}
