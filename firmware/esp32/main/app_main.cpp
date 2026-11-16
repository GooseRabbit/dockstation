#include "esp_log.h"
#include "controller_psk_nvs_credential_store.hpp"
#include "controller_instance_id_provider.hpp"
#include "controller_link_firmware_runtime.hpp"
#include "controller_uart_transport.hpp"
#include "hardware/interfaces/controller_link_startup_readiness.hpp"

namespace {
constexpr char kTag[] = "DockStationESP32";

// This remains default-off until the exact module/SoC, Flash/partition layout,
// UART pinout and electrical compatibility have been verified. No GPIO map is
// guessed or enabled by this scaffold.
dockstation::firmware::esp32::ControllerLinkFirmwareRuntimeStartConfig
    g_controller_link_start_config{};

// Kept for the lifetime of this controller boot. The future runtime start
// receives this provider; the ID itself is not persisted or logged.
dockstation::firmware::esp32::ControllerInstanceIdProvider
    g_controller_instance_id{};
}

extern "C" void app_main(void) {
    using dockstation::hardware::interfaces::ControllerLinkStartupBlocker;
    using dockstation::hardware::interfaces::ControllerLinkStartupReadinessInput;
    using dockstation::hardware::interfaces::
        evaluate_controller_link_startup_readiness;

    ESP_LOGI(kTag, "DockStation ESP32 firmware bootstrap started");

    // Generate the per-boot identity before ADC, I2S or RF subsystems are used.
    if (!g_controller_instance_id.generate_for_current_boot()) {
        ESP_LOGE(kTag, "Could not generate controller instance identity");
        return;
    }
    ESP_LOGI(kTag, "Per-boot controller instance identity generated");

    ControllerLinkStartupReadinessInput readiness_input{};
    readiness_input.hardware_profile_confirmed =
        g_controller_link_start_config.hardware_profile_confirmed;
    readiness_input.uart_configuration_valid =
        dockstation::firmware::esp32::ControllerUartTransport::is_valid_config(
            g_controller_link_start_config.uart);
    // The read-only encrypted-NVS provider is not invoked until production
    // Flash/NVS provisioning and the exact hardware profile are approved.
    readiness_input.pair_credentials_available = false;
    readiness_input.instance_identity_generated =
        g_controller_instance_id.is_generated();
    readiness_input.instance_identity_configuration_matches =
        g_controller_instance_id.is_generated() &&
        (g_controller_link_start_config.session.local_controller_instance_id.empty() ||
         g_controller_link_start_config.session.local_controller_instance_id ==
             g_controller_instance_id.value());
    readiness_input.protocol_version_major =
        g_controller_link_start_config.session.protocol_version.major;
    readiness_input.local_controller_id =
        g_controller_link_start_config.session.local_controller_id;
    readiness_input.peer_controller_id =
        g_controller_link_start_config.session.peer_controller_id;

    const auto readiness =
        evaluate_controller_link_startup_readiness(readiness_input);
    if (!readiness.is_ready()) {
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
            ESP_LOGW(kTag, "ControllerLink blocked: encrypted pair credentials are not loaded");
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

        ESP_LOGW(
            kTag,
            "ControllerLink remains disabled; no NVS credential read, UART driver install or TLS setup was attempted");
        return;
    }

    // This build deliberately stops at readiness evaluation. The application
    // will call ControllerLinkFirmwareRuntime::start() only in a later block,
    // after a reviewed board profile and trusted credential provisioning exist.
    ESP_LOGW(kTag, "ControllerLink prerequisites are ready, but runtime auto-start is disabled");
}

// Link-only acceptance hook. The main component forces this symbol into the
// final ELF so the linker resolves every dependency of the read-only NVS
// provider. app_main deliberately never invokes it.
extern "C" void dockstation_link_check_psk_nvs_credential_store(void) {
    dockstation::firmware::esp32::ControllerPskNvsCredentialStore store;
    dockstation::firmware::esp32::ControllerMbedTlsPskCredentials credentials;
    (void)store.initialize();
    (void)store.load(credentials);
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
