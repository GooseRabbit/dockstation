#include "communications/api_serialization.hpp"
#include "communications/local_api_server.hpp"
#include "platform/linux/platform_capabilities.hpp"

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using dockstation::communications::ApiResponse;
using dockstation::communications::HttpRequest;
using dockstation::communications::LocalApiServeResult;
using dockstation::communications::LocalApiServer;

void test_health_serialization() {
    const std::string payload =
        dockstation::communications::serialize_health_response();
    assert(payload.find("\"api_version\":\"1\"") != std::string::npos);
    assert(payload.find("\"service\":\"dockstation-cpp-gateway\"") != std::string::npos);
    assert(payload.find("\"implementation\":\"cpp\"") != std::string::npos);
    assert(payload.find("\"status\":\"PASS\"") != std::string::npos);
    assert(payload.find("\"read_only\":true") != std::string::npos);
}

void test_diagnostics_serialization() {
    const std::string payload =
        dockstation::communications::serialize_system_diagnostics();
    assert(payload.find("\"schema_version\":\"1.0\"") != std::string::npos);
    assert(payload.find("\"source\":\"LINUX_HOST\"") != std::string::npos);
    assert(payload.find("\"collected_at_utc\":\"") != std::string::npos);
    assert(payload.find("\"summary\":{\"status\":\"WARN\",\"exit_code\":1") !=
           std::string::npos);
    assert(payload.find("\"resources\":") != std::string::npos);
    assert(payload.find("\"memory\":") != std::string::npos);
    assert(payload.find("\"storage\":") != std::string::npos);
    assert(payload.find("\"cpu_temperature\":") != std::string::npos);
    assert(payload.find("\"cpu_load\":") != std::string::npos);
    assert(payload.find("\"default_routes\":") != std::string::npos);
    assert(payload.find("\"dns\":{\"status\":\"NOT_CONFIGURED\"") !=
           std::string::npos);
    assert(payload.find("\"ssh\":{\"status\":\"UNAVAILABLE\"") !=
           std::string::npos);
    assert(payload.find("\"read_only\":true") != std::string::npos);
    assert(payload.find("\"hardware_controller_connected\":false") !=
           std::string::npos);
    assert(payload.find("\"capabilities\":{") != std::string::npos);
    assert(payload.find("\"interfaces\":{") != std::string::npos);
    assert(payload.find("\"candidate_only\":true") != std::string::npos);

    dockstation::platform::linux_platform::ControllerLinkServiceStatus connected{};
    connected.state =
        dockstation::platform::linux_platform::ControllerLinkServiceState::Established;
    connected.tls_authenticated = true;
    connected.protocol_established = true;
    const std::string connected_payload =
        dockstation::communications::serialize_system_diagnostics(&connected);
    assert(connected_payload.find("\"hardware_controller_connected\":true") !=
           std::string::npos);
    assert(payload.find("\"physical_sensors_configured\":false") !=
           std::string::npos);
    assert(payload.find("expected physical sensor inventory/configuration is not yet integrated") !=
           std::string::npos);
    assert(payload.find(":nan") == std::string::npos);
    assert(payload.find(":inf") == std::string::npos);
    assert(payload.find(":-inf") == std::string::npos);
}


void test_platform_capability_serialization() {
    using dockstation::platform::linux_runtime::LinuxPlatformCapabilities;
    using dockstation::platform::linux_runtime::LinuxPlatformKind;
    using dockstation::platform::linux_runtime::PlatformCapabilityStatus;
    LinuxPlatformCapabilities capabilities{};
    capabilities.platform_kind = LinuxPlatformKind::RepkaPi5;
    capabilities.device_tree_model_available = true;
    capabilities.device_tree_compatible_available = true;
    capabilities.kernel_cmdline_available = true;
    (void)std::snprintf(capabilities.device_tree_model.data(),
                        capabilities.device_tree_model.size(), "%s", "Repka Pi 5");
    (void)std::snprintf(capabilities.device_tree_compatible.data(),
                        capabilities.device_tree_compatible.size(), "%s",
                        "repka,repka-pi5;rockchip,rk3588");
    capabilities.uart.status = PlatformCapabilityStatus::Available;
    capabilities.uart.device_node_count = 2U;
    capabilities.uart.accessible_device_count = 1U;
    capabilities.uart.sysfs_controller_count = 2U;
    capabilities.uart.kernel_console_conflict_count = 1U;
    (void)std::snprintf(capabilities.uart.first_device_path.data(),
                        capabilities.uart.first_device_path.size(), "%s", "/dev/ttyS0");
    capabilities.gpio.status = PlatformCapabilityStatus::NotConfigured;
    capabilities.gpio.sysfs_controller_count = 1U;
    capabilities.i2c.status = PlatformCapabilityStatus::Unavailable;
    capabilities.spi.status = PlatformCapabilityStatus::Unknown;
    capabilities.pwm.status = PlatformCapabilityStatus::Detected;
    capabilities.pwm.device_node_count = 1U;
    capabilities.camera.status = PlatformCapabilityStatus::Available;
    capabilities.camera.device_node_count = 1U;
    capabilities.camera.accessible_device_count = 1U;
    const std::string payload =
        dockstation::communications::serialize_system_diagnostics(nullptr, &capabilities);
    assert(payload.find("\"profile\":\"REPKA_PI5\"") != std::string::npos);
    assert(payload.find("\"model\":\"Repka Pi 5\"") != std::string::npos);
    assert(payload.find("\"compatible\":\"repka,repka-pi5;rockchip,rk3588\"") !=
           std::string::npos);
    assert(payload.find("\"status\":\"AVAILABLE\",\"device_nodes\":2") !=
           std::string::npos);
    assert(payload.find("\"candidate_device_path\":\"/dev/ttyS0\"") !=
           std::string::npos);
    assert(payload.find("\"candidate_only\":true") != std::string::npos);
    assert(payload.find("\"kernel_console_conflicts\":1") != std::string::npos);
    assert(payload.find("\"gpio\":{\"status\":\"NOT_CONFIGURED\"") !=
           std::string::npos);
    assert(payload.find("\"spi\":{\"status\":\"UNKNOWN\"") != std::string::npos);
}

void test_controller_telemetry_serialization() {
    using dockstation::hardware::interfaces::ControllerCodecDecodedMessage;
    using dockstation::hardware::interfaces::ControllerMessageType;
    using dockstation::hardware::interfaces::ControllerProtocolVersion;
    using dockstation::hardware::interfaces::ControllerSensorValue;
    using dockstation::hardware::interfaces::ControllerTelemetryMeasurement;
    using dockstation::hardware::interfaces::ControllerTelemetrySnapshot;
    using dockstation::hardware::interfaces::ControllerTelemetrySnapshotPayload;
    using dockstation::hardware::interfaces::SensorQualityStatus;

    // Eight logical station-state values plus four independent DHT22 channels.
    const std::array<ControllerTelemetryMeasurement, 12U> measurements{{
        {"lid.position", std::string_view{"NOT_CONFIGURED"}, "status",
         SensorQualityStatus::NotConfigured, "mono_us_100"},
        {"platform.motion_active", std::string_view{"UNKNOWN"}, "status",
         SensorQualityStatus::Unknown, "mono_us_100"},
        {"cleaning.active", std::string_view{"UNKNOWN"}, "status",
         SensorQualityStatus::Unknown, "mono_us_100"},
        {"drive.motion_active", std::string_view{"UNKNOWN"}, "status",
         SensorQualityStatus::Unknown, "mono_us_100"},
        {"safety_stop.active", std::string_view{"UNKNOWN"}, "status",
         SensorQualityStatus::Unknown, "mono_us_100"},
        {"workspace.state", std::string_view{"UNKNOWN"}, "status",
         SensorQualityStatus::Unknown, "mono_us_100"},
        {"command.last_decision", std::string_view{"NONE"}, "status",
         SensorQualityStatus::Unknown, "mono_us_100"},
        {"command.pending", std::string_view{"NONE"}, "status",
         SensorQualityStatus::Unknown, "mono_us_100"},
        {"dht22_1.temperature", 21.7, "°C", SensorQualityStatus::Ok, "mono_us_100"},
        {"dht22_1.humidity", 46.2, "%", SensorQualityStatus::Ok, "mono_us_100"},
        {"dht22_2.temperature", std::string_view{"NOT_CONFIGURED"}, "status",
         SensorQualityStatus::NotConfigured, "mono_us_100"},
        {"dht22_2.humidity", std::string_view{"NOT_CONFIGURED"}, "status",
         SensorQualityStatus::NotConfigured, "mono_us_100"},
    }};
    ControllerCodecDecodedMessage decoded{};
    decoded.envelope.type = ControllerMessageType::TelemetrySnapshot;
    decoded.envelope.protocol_version = ControllerProtocolVersion{1U, 0U};
    decoded.envelope.controller_id = "esp32-01";
    decoded.envelope.controller_instance_id = "boot-01";
    decoded.envelope.sequence_number = 17U;
    decoded.envelope.sent_at = "1794800000000";
    decoded.payload = ControllerTelemetrySnapshotPayload{
        measurements.data(), measurements.size()};

    ControllerTelemetrySnapshot snapshot{};
    assert(snapshot.replace(decoded, 1794800000123ULL));
    const auto payload = dockstation::communications::serialize_system_diagnostics(
        nullptr, nullptr, &snapshot);
    assert(payload.find("\"controller_telemetry\":{") != std::string::npos);
    assert(payload.find("\"status\":\"WARN\"") != std::string::npos);
    assert(payload.find("\"source_controller_id\":\"esp32-01\"") != std::string::npos);
    assert(payload.find("\"sequence_number\":17") != std::string::npos);
    assert(payload.find("\"received_at_utc\":\"2026-11-16T") != std::string::npos);
    assert(snapshot.measurement_count() == 12U);
    assert(payload.find("\"sensor_id\":\"lid.position\"") != std::string::npos);
    assert(payload.find("\"sensor_id\":\"workspace.state\"") != std::string::npos);
    assert(payload.find("\"sensor_id\":\"command.pending\"") != std::string::npos);
    assert(payload.find("\"sensor_id\":\"dht22_1.temperature\"") != std::string::npos);
    assert(payload.find("\"sensor_id\":\"dht22_1.humidity\"") != std::string::npos);
    assert(payload.find("\"sensor_id\":\"dht22_2.temperature\"") != std::string::npos);
    assert(payload.find("\"sensor_id\":\"dht22_2.humidity\"") != std::string::npos);
    assert(payload.find("\"value_type\":\"NUMBER\",\"value\":21.7") != std::string::npos);
    assert(payload.find("\"value_type\":\"NUMBER\",\"value\":46.2") != std::string::npos);
    assert(payload.find("\"quality_status\":\"OK\"") != std::string::npos);
    assert(payload.find("\"quality_status\":\"UNKNOWN\"") != std::string::npos);
    assert(payload.find("\"quality_status\":\"NOT_CONFIGURED\"") != std::string::npos);
    assert(payload.find("\"value\":\"NOT_CONFIGURED\"") != std::string::npos);
    assert(payload.find("\"sensor_id\":\"dht22_1.temperature\"") != std::string::npos);
    assert(payload.find("\"sensor_id\":\"dht22_1.humidity\"") != std::string::npos);
    assert(payload.find("\"sensor_id\":\"dht22_2.temperature\"") != std::string::npos);
    assert(payload.find("\"sensor_id\":\"dht22_2.humidity\"") != std::string::npos);
    assert(payload.find("\"quality_status\":\"NOT_CONFIGURED\"") != std::string::npos);
    assert(payload.find("\"value\":\"NOT_CONFIGURED\"") != std::string::npos);

    dockstation::platform::linux_platform::ControllerLinkServiceStatus linked{};
    linked.tls_authenticated = true;
    linked.protocol_established = true;
    const auto live_payload =
        dockstation::communications::serialize_system_diagnostics(
            &linked, nullptr, &snapshot);
    assert(live_payload.find("\"controller_telemetry\":{\"status\":\"PASS\"") !=
           std::string::npos);

    dockstation::platform::linux_platform::ControllerLinkServiceStatus busy{};
    busy.state = dockstation::platform::linux_platform::ControllerLinkServiceState::LinkLost;
    busy.error_code = "CONTROLLER_UART_DEVICE_BUSY";
    const auto busy_payload =
        dockstation::communications::serialize_system_diagnostics(&busy);
    assert(busy_payload.find("\"controller_link\":{\"status\":\"LINK_LOST\"") !=
           std::string::npos);
    assert(busy_payload.find("\"error_code\":\"CONTROLLER_UART_DEVICE_BUSY\"") !=
           std::string::npos);

    const auto unavailable =
        dockstation::communications::serialize_system_diagnostics();
    assert(unavailable.find(
        "\"controller_telemetry\":{\"status\":\"NOT_CONFIGURED\"") !=
        std::string::npos);
}

void test_routes() {
    ApiResponse response =
        dockstation::communications::dispatch_api_request(
            HttpRequest{"GET", "/api/v1/health", "HTTP/1.1"});
    assert(response.status_code == 200U);
    assert(response.body.find("\"read_only\":true") != std::string::npos);

    response = dockstation::communications::dispatch_api_request(
        HttpRequest{"GET", "/api/v1/controller/link", "HTTP/1.1"});
    assert(response.status_code == 200U);
    assert(response.body.find("\"status\":\"NOT_CONFIGURED\"") != std::string::npos);
    assert(response.body.find("\"state\":\"NOT_CONFIGURED\"") != std::string::npos);
    assert(response.body.find("\"tls_authenticated\":false") != std::string::npos);

    response = dockstation::communications::dispatch_api_request(
        HttpRequest{"POST", "/api/v1/health", "HTTP/1.1"});
    assert(response.status_code == 405U);
    assert(response.allow == "GET");

    response = dockstation::communications::dispatch_api_request(
        HttpRequest{"GET", "/not-found", "HTTP/1.1"});
    assert(response.status_code == 404U);

    response = dockstation::communications::dispatch_api_request(
        HttpRequest{"GET", "/api/v1/health?debug=1", "HTTP/1.1"});
    assert(response.status_code == 400U);
}

void test_request_parser() {
    auto parsed = dockstation::communications::parse_http_request(
        "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(parsed.valid);
    assert(parsed.request.method == "GET");
    assert(parsed.request.target == "/api/v1/health");

    parsed = dockstation::communications::parse_http_request(
        "GET /api/v1/health HTTP/1.1\r\nContent-Length: 1\r\n\r\nx");
    assert(!parsed.valid);

    parsed = dockstation::communications::parse_http_request(
        "GET /api/v1/health HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n");
    assert(!parsed.valid);

    parsed = dockstation::communications::parse_http_request(
        "GET /api/v1/health HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    assert(!parsed.valid);

    parsed = dockstation::communications::parse_http_request(
        "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\n\r\nbody");
    assert(!parsed.valid);

    parsed = dockstation::communications::parse_http_request(
        "GET /api/v1/health HTTP/1.1\r\nMalformed Header: x\r\n\r\n");
    assert(!parsed.valid);

    const std::string oversized_request(8193U, 'A');
    parsed = dockstation::communications::parse_http_request(oversized_request);
    assert(!parsed.valid);
    assert(parsed.too_large);
}

void test_eight_concurrent_clients() {
    LocalApiServer server;
    assert(server.start(0U));
    const auto status = server.status();
    std::atomic<bool> finish_server{false};
    std::thread server_thread([&server, &finish_server]() {
        while (!finish_server.load()) {
            (void)server.serve_once(25);
        }
    });

    constexpr std::size_t client_count = 8U;
    std::array<int, client_count> clients{};
    const std::string request =
        "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    for (std::size_t i = 0; i < client_count; ++i) {
        clients[i] = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        assert(clients[i] >= 0);
        struct sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(status.port);
        assert(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
        assert(::connect(clients[i],
            reinterpret_cast<const struct sockaddr*>(&address),
            sizeof(address)) == 0);
        assert(::send(clients[i], request.data(), request.size(), 0) ==
               static_cast<ssize_t>(request.size()));
    }

    for (int client : clients) {
        struct timeval timeout {};
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;
        assert(::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                            &timeout, sizeof(timeout)) == 0);
        std::string response;
        char buffer[512];
        for (;;) {
            const ssize_t count = ::recv(client, buffer, sizeof(buffer), 0);
            if (count == 0) {
                break;
            }
            assert(count > 0);
            response.append(buffer, static_cast<std::size_t>(count));
        }
        assert(response.find("HTTP/1.1 200 OK") != std::string::npos);
        assert(response.find("dockstation-cpp-gateway") != std::string::npos);
        ::close(client);
    }

    finish_server.store(true);
    server_thread.join();
    server.stop();
}

void test_loopback_server() {
    LocalApiServer server;
    assert(server.start(0U));
    const auto status = server.status();
    assert(status.running);
    assert(status.bind_address == "127.0.0.1");
    assert(status.port != 0U);

    std::atomic<bool> finish_server{false};
    std::thread server_thread([&server, &finish_server]() {
        while (!finish_server.load()) {
            (void)server.serve_once(50);
        }
    });

    // An idle client must not hold the event loop while a second client
    // completes a normal request.
    const int slow_client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(slow_client >= 0);
    struct timeval slow_timeout {};
    slow_timeout.tv_sec = 3;
    slow_timeout.tv_usec = 0;
    assert(::setsockopt(slow_client, SOL_SOCKET, SO_RCVTIMEO,
                        &slow_timeout, sizeof(slow_timeout)) == 0);
    struct sockaddr_in slow_address {};
    slow_address.sin_family = AF_INET;
    slow_address.sin_port = htons(status.port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &slow_address.sin_addr) == 1);
    assert(::connect(slow_client,
        reinterpret_cast<const struct sockaddr*>(&slow_address),
        sizeof(slow_address)) == 0);

    const int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(client >= 0);
    struct timeval timeout {};
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    assert(::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                        &timeout, sizeof(timeout)) == 0);
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(status.port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(::connect(client, reinterpret_cast<const struct sockaddr*>(&address),
                     sizeof(address)) == 0);

    const std::string request =
        "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::size_t sent_total = 0U;
    while (sent_total < request.size()) {
        const ssize_t sent = ::send(
            client, request.data() + sent_total, request.size() - sent_total, 0);
        assert(sent > 0);
        sent_total += static_cast<std::size_t>(sent);
    }

    std::string response;
    char buffer[512];
    for (;;) {
        const ssize_t count = ::recv(client, buffer, sizeof(buffer), 0);
        if (count == 0) {
            break;
        }
        assert(count > 0);
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(client);
    finish_server.store(true);
    server_thread.join();

    assert(response.find("HTTP/1.1 200 OK") != std::string::npos);
    assert(response.find("Content-Type: application/json; charset=utf-8") !=
           std::string::npos);
    assert(response.find("\"service\":\"dockstation-cpp-gateway\"") !=
           std::string::npos);
    assert(response.find("\"implementation\":\"cpp\"") !=
           std::string::npos);

    server.stop();
    char shutdown_byte = 0;
    assert(::recv(slow_client, &shutdown_byte, 1U, 0) == 0);
    ::close(slow_client);
    assert(!server.status().running);
}

}  // namespace

int main() {
    test_health_serialization();
    test_diagnostics_serialization();
    test_platform_capability_serialization();
    test_controller_telemetry_serialization();
    test_routes();
    test_request_parser();
    test_eight_concurrent_clients();
    test_loopback_server();
    std::cout << "Local API tests passed\n";
    return 0;
}
