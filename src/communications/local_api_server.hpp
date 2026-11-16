#pragma once

#include <csignal>
#include <cstdint>
#include <string>

#include "communications/network_types.hpp"
#include "platform/linux/controller_link_service.hpp"

namespace dockstation::communications {

[[nodiscard]] HttpParseResult parse_http_request(const std::string& raw_request);
[[nodiscard]] ApiResponse dispatch_api_request(
    const HttpRequest& request,
    const platform::linux_platform::ControllerLinkService* controller_link = nullptr);

class LocalApiServer {
public:
    LocalApiServer() = default;
    LocalApiServer(const LocalApiServer&) = delete;
    LocalApiServer& operator=(const LocalApiServer&) = delete;
    ~LocalApiServer();

    // Port 0 requests an ephemeral port and is intended for host tests only.
    [[nodiscard]] bool start(
        std::uint32_t port,
        platform::linux_platform::ControllerLinkService* controller_link = nullptr);
    [[nodiscard]] LocalApiServeResult serve_once(int timeout_milliseconds);
    void stop() noexcept;
    [[nodiscard]] LocalApiServerStatus status() const;

private:
    int listen_descriptor_{-1};
    std::uint16_t bound_port_{0U};
    std::string error_{};
    platform::linux_platform::ControllerLinkService* controller_link_{nullptr};
};

[[nodiscard]] int run_local_api_server(
    std::uint32_t port,
    const volatile std::sig_atomic_t* shutdown_requested,
    platform::linux_platform::ControllerLinkServiceConfig controller_config = {});

// Composition-root overload: the caller may attach a trusted message handler
// before this function starts the externally owned ControllerLink service.
[[nodiscard]] int run_local_api_server(
    std::uint32_t port,
    const volatile std::sig_atomic_t* shutdown_requested,
    platform::linux_platform::ControllerLinkService& controller_link,
    platform::linux_platform::ControllerLinkServiceConfig controller_config);

}  // namespace dockstation::communications
