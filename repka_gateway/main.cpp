#include <cstddef>
#include <csignal>
#include <string>
#include <vector>

#include "application/linux/application_bootstrap.hpp"
#include "communications/authenticated_event_transport_adapter.hpp"
#include "communications/controller_subsystem_status_event_projector.hpp"
#include "communications/local_api_server.hpp"
#include "core/source_session_verifier.hpp"
#include "core/station_event_pipeline.hpp"
#include "core/subsystem_status_event_processor.hpp"
#include "platform/linux/controller_link_service.hpp"
#include "platform/linux/runtime_config.hpp"
#include "platform/logging/logger.hpp"

#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
#include "application/linux/gateway_persistence_context.hpp"
#endif

namespace {

#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
class UnsupportedGatewayEventProcessor final
    : public dockstation::core::EventProcessorPort {
public:
    [[nodiscard]] dockstation::core::EventProcessorStatus process(
        const dockstation::core::Event&,
        dockstation::core::EventCategory,
        const dockstation::core::SourceCursor&) noexcept override {
        return dockstation::core::EventProcessorStatus::Unsupported;
    }
};

int run_gateway_with_persistent_event_ingress(
    const dockstation::platform::linux_runtime::RuntimeConfig& config,
    const volatile std::sig_atomic_t* shutdown_requested,
    const dockstation::platform::linux_platform::ControllerLinkServiceConfig&
        controller_config,
    dockstation::application::linux_app::GatewayPersistenceContext& persistence) {
    using dockstation::communications::AuthenticatedEventTransportAdapter;
    using dockstation::communications::ControllerSubsystemStatusEventProjector;
    using dockstation::core::EventValidationPolicy;
    using dockstation::core::RegistrySourceSessionVerifier;
    using dockstation::core::StationEventPipeline;
    using dockstation::core::SubsystemStatusEventProcessor;
    using dockstation::platform::linux_platform::ControllerLinkService;
    using dockstation::platform::logging::LogLevel;
    using dockstation::platform::logging::write_log_record;

    if (config.station_id.empty() || config.controller_source.empty()) {
        (void)write_log_record(
            LogLevel::Warning, "gateway_composition",
            "Аутентифицированный приём событий подсистем не настроен",
            {{"event_ingress", "NOT_CONFIGURED"},
             {"reason", "station_id/controller_source are not configured"}});
        return dockstation::communications::run_local_api_server(
            config.api_port, shutdown_requested, controller_config);
    }

    int service_result = 1;

    // The ControllerLink event callback is synchronous. Holding this gate for
    // the service lifetime ensures that every pipeline/repository call uses
    // the same connection and the same adapter-owned string buffers serially.
    const bool invoked = persistence.with_serialized_stores(
        [&](dockstation::platform::persistence::SqliteSourceSessionStore&
                source_store,
            dockstation::platform::persistence::SqliteSubsystemStatusStore&
                status_store) {
            RegistrySourceSessionVerifier source_verifier{
                source_store, config.station_id};

            ControllerSubsystemStatusEventProjector event_codec{};
            SubsystemStatusEventProcessor subsystem_status_processor{
                config.station_id, event_codec, status_store, status_store};

            UnsupportedGatewayEventProcessor global_processor{};
            UnsupportedGatewayEventProcessor operation_processor{};
            UnsupportedGatewayEventProcessor timeout_processor{};

            StationEventPipeline pipeline{
                status_store,
                global_processor,
                operation_processor,
                timeout_processor,
                source_verifier,
                source_store,
                source_store,
                &subsystem_status_processor};

            ControllerLinkService controller_link{};
            AuthenticatedEventTransportAdapter event_adapter{
                controller_link,
                pipeline,
                event_codec,
                config.controller_source,
                EventValidationPolicy{true, true, false}};

            if (!event_adapter.is_configured()) {
                (void)write_log_record(
                    LogLevel::Error, "gateway_composition",
                    "Не удалось подключить аутентифицированный приём событий",
                    {{"event_ingress", "ERROR"},
                     {"station_id", config.station_id},
                     {"controller_source", config.controller_source}});
                service_result = 1;
                return;
            }

            (void)write_log_record(
                LogLevel::Info, "gateway_composition",
                "Аутентифицированный приём статусов подсистем подготовлен",
                {{"station_id", config.station_id},
                 {"controller_source", config.controller_source},
                 {"route", "SUBSYSTEM_STATUS_UPDATED"}});

            service_result = dockstation::communications::run_local_api_server(
                config.api_port, shutdown_requested, controller_link,
                controller_config);
        });

    if (!invoked) {
        (void)write_log_record(
            LogLevel::Error, "gateway_composition",
            "Хранилище закрылось до запуска аутентифицированного приёма событий");
        return 1;
    }
    return service_result;
}
#endif

int run_gateway_service(
    const dockstation::platform::linux_runtime::RuntimeConfig& config,
    const volatile std::sig_atomic_t* shutdown_requested) {
    using dockstation::platform::logging::LogLevel;
    using dockstation::platform::logging::write_log_record;

    dockstation::platform::linux_platform::ControllerLinkServiceConfig
        controller_config{
            config.controller_uart_device,
            config.controller_local_id,
            config.controller_peer_id};

#if defined(DOCKSTATION_WITH_SQLITE_PERSISTENCE)
    // This owner outlives the controller channel and HTTP server. Both stores
    // share its one connection and the event pipeline enters them only through
    // GatewayPersistenceContext::with_serialized_stores().
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

    const int service_result = run_gateway_with_persistent_event_ingress(
        config, shutdown_requested, controller_config, persistence);

    if (!persistence.close()) {
        (void)write_log_record(
            LogLevel::Error, "gateway_composition",
            "Не удалось штатно закрыть локальное хранилище DockStation");
        return 1;
    }
    (void)write_log_record(
        LogLevel::Info, "gateway_composition",
        "Локальное хранилище DockStation закрыто");
    return service_result;
#else
    (void)write_log_record(
        LogLevel::Warning, "gateway_composition",
        "Сборка выполнена без SQLite; постоянное хранилище недоступно",
        {{"persistence", "NOT_CONFIGURED"},
         {"event_ingress", "NOT_CONFIGURED"}});
    return dockstation::communications::run_local_api_server(
        config.api_port, shutdown_requested, controller_config);
#endif
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
