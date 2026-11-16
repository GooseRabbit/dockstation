#include "application/linux/gateway_persistence_context.hpp"

namespace dockstation::application::linux_app {

GatewayPersistenceContext::GatewayPersistenceContext() noexcept
    : source_session_store_{database_},
      subsystem_status_store_{database_} {}

platform::persistence::SqliteDatabaseResult GatewayPersistenceContext::open(
    std::string_view database_path) noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    return database_.open(platform::persistence::SqliteDatabaseConfig{
        database_path, kGatewayDatabaseBusyTimeoutMilliseconds});
}

bool GatewayPersistenceContext::close() noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    return database_.close();
}

bool GatewayPersistenceContext::is_open() const noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    return database_.is_open();
}

int GatewayPersistenceContext::schema_version() const noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    return database_.schema_version();
}

}  // namespace dockstation::application::linux_app
