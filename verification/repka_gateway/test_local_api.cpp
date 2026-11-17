#include "communications/api_serialization.hpp"
#include "communications/local_api_server.hpp"
#include "platform/linux/platform_capabilities.hpp"

#include <arpa/inet.h>
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

    const std::array<ControllerTelemetryMeasurement, 5U> measurements{{
        {"dht22-01.temperature", 23.75, "degC", SensorQualityStatus::Ok, "m-001"},
        {"dht22-01.humidity", std::uint64_t{61U}, "%", SensorQualityStatus::Stale, "m-002"},
        {"sensor.count", std::int64_t{-7}, "count", SensorQualityStatus::Ok, "m-005"},
        {"cover.closed", true, "bool", SensorQualityStatus::Unknown, "m-003"},
        {"sensor.note", std::string_view{"probe\"ok"}, "text", SensorQualityStatus::Invalid, "m-004"},
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
    assert(payload.find("\"sensor_id\":\"dht22-01.temperature\"") != std::string::npos);
    assert(payload.find("\"value_type\":\"NUMBER\",\"value\":23.75") != std::string::npos);
    assert(payload.find("\"value_type\":\"UNSIGNED_INTEGER\",\"value\":61") != std::string::npos);
    assert(payload.find("\"value_type\":\"INTEGER\",\"value\":-7") != std::string::npos);
    assert(payload.find("\"value_type\":\"BOOLEAN\",\"value\":true") != std::string::npos);
    assert(payload.find("probe\\\"ok") != std::string::npos);
    assert(payload.find("\"quality_status\":\"STALE\"") != std::string::npos);

    dockstation::platform::linux_platform::ControllerLinkServiceStatus linked{};
    linked.tls_authenticated = true;
    linked.protocol_established = true;
    const auto live_payload =
        dockstation::communications::serialize_system_diagnostics(
            &linked, nullptr, &snapshot);
    assert(live_payload.find("\"controller_telemetry\":{\"status\":\"PASS\"") !=
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
}

void test_loopback_server() {
    LocalApiServer server;
    assert(server.start(0U));
    const auto status = server.status();
    assert(status.running);
    assert(status.bind_address == "127.0.0.1");
    assert(status.port != 0U);

    LocalApiServeResult serve_result = LocalApiServeResult::Error;
    std::thread server_thread([&server, &serve_result]() {
        serve_result = server.serve_once(5000);
    });

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
    server_thread.join();

    assert(serve_result == LocalApiServeResult::Handled);
    assert(response.find("HTTP/1.1 200 OK") != std::string::npos);
    assert(response.find("Content-Type: application/json; charset=utf-8") !=
           std::string::npos);
    assert(response.find("\"service\":\"dockstation-cpp-gateway\"") !=
           std::string::npos);
    assert(response.find("\"implementation\":\"cpp\"") !=
           std::string::npos);

    server.stop();
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
    test_loopback_server();
    std::cout << "Local API tests passed\n";
    return 0;
}
