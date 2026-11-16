#pragma once

#include <cstdint>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>

#include "platform/persistence/sqlite_database.hpp"
#include "platform/persistence/sqlite_source_session_store.hpp"
#include "platform/persistence/sqlite_subsystem_status_store.hpp"

namespace dockstation::application::linux_app {

inline constexpr char kGatewayDatabasePath[] =
    "/var/lib/dockstation/dockstation.sqlite3";
inline constexpr std::int32_t kGatewayDatabaseBusyTimeoutMilliseconds = 1000;

// Composition-root owner for the one local SQLite connection and both
// production adapters. The database is declared first so it is destroyed
// after the adapters that hold references to it.
class GatewayPersistenceContext final {
public:
    GatewayPersistenceContext() noexcept;
    ~GatewayPersistenceContext() = default;

    GatewayPersistenceContext(const GatewayPersistenceContext&) = delete;
    GatewayPersistenceContext& operator=(const GatewayPersistenceContext&) = delete;
    GatewayPersistenceContext(GatewayPersistenceContext&&) = delete;
    GatewayPersistenceContext& operator=(GatewayPersistenceContext&&) = delete;

    // The path is explicit for host tests; production uses the DEC-068 path.
    // open/close are mutually exclusive with adapter access.
    [[nodiscard]] platform::persistence::SqliteDatabaseResult open(
        std::string_view database_path = kGatewayDatabasePath) noexcept;
    [[nodiscard]] bool close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] int schema_version() const noexcept;

    // Runs one void callback while holding the context mutex.
    // All SQLite adapter calls must happen inside this gate. Borrowed string_views
    // returned by readers must be consumed/copied before the callback returns.
    // Callbacks must not nest context operations. Closed DB means no invocation.
    template <typename Callback>
    [[nodiscard]] bool with_serialized_stores(Callback&& callback) {
        using CallbackResult = std::invoke_result_t<
            Callback,
            platform::persistence::SqliteSourceSessionStore&,
            platform::persistence::SqliteSubsystemStatusStore&>;
        static_assert(std::is_void_v<CallbackResult>,
                      "Serialized store callbacks must not return borrowed data");

        const std::lock_guard<std::mutex> lock{mutex_};
        if (!database_.is_open()) {
            return false;
        }
        std::forward<Callback>(callback)(source_session_store_,
                                         subsystem_status_store_);
        return true;
    }

private:
    // Declaration order guarantees adapters are destroyed before database_.
    platform::persistence::SqliteDatabase database_;
    platform::persistence::SqliteSourceSessionStore source_session_store_;
    platform::persistence::SqliteSubsystemStatusStore subsystem_status_store_;
    mutable std::mutex mutex_;
};

}  // namespace dockstation::application::linux_app
