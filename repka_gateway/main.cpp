#include <cstddef>
#include <csignal>
#include <string>
#include <vector>

#include "application/linux/application_bootstrap.hpp"
#include "communications/local_api_server.hpp"
#include "platform/linux/runtime_config.hpp"
#include "platform/logging/logger.hpp"

#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
#include "application/linux/gateway_persistence_context.hpp"
#endif

namespace {

int run_gateway_service(
    const dockstation::platform::linux_runtime::RuntimeConfig& config,
    const volatile std::sig_atomic_t* shutdown_requested) {
    using dockstation::platform::logging::LogLevel;
    using dockstation::platform::logging::write_log_record;

#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
    // This owner outlives the controller channel and HTTP server. Both stores
    // share its one connection and are accessed only through its mutex gate.
    dockstation::application::linux_app::GatewayPersistenceContext persistence;
    const auto persistence_result = persistence.open();
    if (!persistence_result) {
        (void)write_log_record(
            LogLevel::Error, "gateway_composition",
            "Не удалось открыть локальное хранилище DockStation",
            {{"database_path",
              dockstation::application::linux_app::kGatewayDatabasePath},
             {"database_status",
              std::to_string(static_cast<int>(persistence_result.status))},
             {"sqlite_status", std::to_string(persistence_result.sqlite_status)}});
        return 1;
    }

    (void)write_log_record(
        LogLevel::Info, "gateway_composition",
        "Локальное хранилище DockStation открыто",
        {{"database_path",
          dockstation::application::linux_app::kGatewayDatabasePath},
         {"schema_version", std::to_string(persistence.schema_version())}});
#else
    (void)write_log_record(
        LogLevel::Warning, "gateway_composition",
        "Сборка выполнена без SQLite; постоянное хранилище недоступно",
        {{"persistence", "NOT_CONFIGURED"}});
#endif

    dockstation::platform::linux_platform::ControllerLinkServiceConfig controller_config{
        config.controller_uart_device,
        config.controller_local_id,
        config.controller_peer_id};
    const int service_result =
        dockstation::communications::run_local_api_server(
            config.api_port, shutdown_requested, controller_config);

#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
    if (!persistence.close()) {
        (void)write_log_record(
            LogLevel::Error, "gateway_composition",
            "Не удалось штатно закрыть локальное хранилище DockStation");
        return 1;
    }
    (void)write_log_record(
        LogLevel::Info, "gateway_composition",
        "Локальное хранилище DockStation закрыто");
#endif
    return service_result;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::vector<const char*> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(argv[index]);
    }

    const auto result =
        dockstation::application::linux_app::run_application(
            argc, arguments.data(), run_gateway_service);
    return static_cast<int>(result.exit_code);
}
