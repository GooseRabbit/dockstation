#include "communications/api_serialization.hpp"
#include "communications/local_api_server.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <cstddef>
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
    assert(payload.find("\"service\":\"dockstation-gateway\"") != std::string::npos);
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
    assert(payload.find("\"physical_sensors_configured\":false") !=
           std::string::npos);
    assert(payload.find(":nan") == std::string::npos);
    assert(payload.find(":inf") == std::string::npos);
    assert(payload.find(":-inf") == std::string::npos);
}

void test_routes() {
    ApiResponse response =
        dockstation::communications::dispatch_api_request(
            HttpRequest{"GET", "/api/v1/health", "HTTP/1.1"});
    assert(response.status_code == 200U);
    assert(response.body.find("\"read_only\":true") != std::string::npos);

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
    assert(response.find("\"service\":\"dockstation-gateway\"") !=
           std::string::npos);

    server.stop();
    assert(!server.status().running);
}

}  // namespace

int main() {
    test_health_serialization();
    test_diagnostics_serialization();
    test_routes();
    test_request_parser();
    test_loopback_server();
    std::cout << "Local API tests passed\n";
    return 0;
}
