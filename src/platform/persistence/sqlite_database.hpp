#pragma once

#include <cstdint>
#include <string_view>

struct sqlite3;

namespace dockstation::platform::persistence {

enum class SqliteDatabaseStatus : std::uint8_t {
    Ready,
    InvalidPath,
    InvalidBusyTimeout,
    OpenFailed,
    SettingsFailed,
    ApplicationIdMismatch,
    SchemaVersionUnsupported,
    SchemaMigrationFailed,
    SchemaInvalid
};

struct SqliteDatabaseConfig final {
    std::string_view database_path{};
    std::int32_t busy_timeout_ms{1000};
};

struct SqliteDatabaseResult final {
    SqliteDatabaseStatus status{SqliteDatabaseStatus::OpenFailed};
    int sqlite_status{0};
    int schema_version{0};

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == SqliteDatabaseStatus::Ready;
    }
};

// Owns one SQLite connection for the station-local persistence backend.
// Database calls must be serialized by the composition root. FULLMUTEX is
// enabled as a second line of defence; it does not replace repository-level
// transaction boundaries or the single-writer policy.
class SqliteDatabase final {
public:
    static constexpr int kCurrentSchemaVersion = 1;

    SqliteDatabase() noexcept = default;
    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;
    SqliteDatabase(SqliteDatabase&&) = delete;
    SqliteDatabase& operator=(SqliteDatabase&&) = delete;

    [[nodiscard]] SqliteDatabaseResult open(
        const SqliteDatabaseConfig& config) noexcept;
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] int schema_version() const noexcept;
    [[nodiscard]] int last_sqlite_status() const noexcept;

    // Restricted to the Linux persistence adapter layer; core interfaces
    // never include SQLite types or depend on this native handle.
    [[nodiscard]] sqlite3* native_handle() noexcept;

private:
    sqlite3* database_{nullptr};
    int schema_version_{0};
    int last_sqlite_status_{0};
};

}  // namespace dockstation::platform::persistence
