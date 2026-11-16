#include "esp_err.h"
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

// Owns the stable IDs because the runtime configuration holds string_views
// into these buffers. They remain valid for the full application lifetime.
dockstation::hardware::interfaces::ControllerStableIdPair
    g_controller_stable_ids{};

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
}  // namespace

extern "C" void app_main(void) {
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

    // The default hardware profile is deliberately unconfirmed. This check
    // happens before NVS initialization: an unknown board must not touch the
    // production credential store or attempt any transport setup.
    if (!g_controller_link_start_config.hardware_profile_confirmed) {
        ESP_LOGW(kTag, "ControllerLink remains NOT_CONFIGURED: physical hardware profile is not confirmed; secure NVS was not opened");
        return;
    }

    // Require an explicit, non-console UART pin map before attempting any
    // credential access. The UART driver itself is still not installed here.
    if (!dockstation::firmware::esp32::ControllerUartTransport::is_valid_config(
            g_controller_link_start_config.uart)) {
        ESP_LOGW(kTag, "ControllerLink remains NOT_CONFIGURED: explicit UART port/TX/RX configuration is missing or invalid; secure NVS was not opened");
        return;
    }

    // Read only pre-provisioned credentials from an existing encrypted NVS
    // partition. The store never generates, repairs, writes or rotates keys.
    dockstation::firmware::esp32::ControllerPskNvsCredentialStore
        credential_store{};
    if (!credential_store.initialize()) {
        ESP_LOGW(
            kTag,
            "ControllerLink remains NOT_CONFIGURED: protected NVS is unavailable (%s)",
            esp_err_to_name(credential_store.last_error()));
        return;
    }

    dockstation::firmware::esp32::ControllerMbedTlsPskCredentials credentials{};
    if (!credential_store.load(credentials)) {
        ESP_LOGW(
            kTag,
            "ControllerLink remains NOT_CONFIGURED: pre-provisioned PSK/identity are unavailable (%s)",
            esp_err_to_name(credential_store.last_error()));
        credentials.clear();
        return;
    }

    if (!credential_store.load_controller_ids(g_controller_stable_ids)) {
        ESP_LOGW(
            kTag,
            "ControllerLink remains NOT_CONFIGURED: stable local/peer controller IDs are unavailable or invalid (%s)",
            esp_err_to_name(credential_store.last_error()));
        credentials.clear();
        g_controller_stable_ids.clear();
        return;
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
        credentials.clear();
        return;
    }

    // Do not start the link yet. The FreeRTOS polling/lifecycle task and the
    // real telemetry sampler are separate implementation slices; starting
    // the runtime without them would leave a live transport unserviced.
    credentials.clear();
    ESP_LOGW(
        kTag,
        "ControllerLink prerequisites are valid; runtime remains stopped until the application polling task and real telemetry source are integrated");
}

// Link-only acceptance hook. The main component forces this symbol into the
// final ELF so the linker resolves every dependency of the read-only NVS
// provider. app_main invokes it only indirectly through the gated read path.
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
