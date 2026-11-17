#include "platform/persistence/sqlite_database.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sqlite3.h>

namespace dockstation::platform::persistence {
namespace {

constexpr std::size_t kMaximumDatabasePathBytes = 4095U;
constexpr int kDockStationApplicationId = 0x4453544E;  // "DSTN"
constexpr int kInitialSchemaVersion = 1;
constexpr int kSourceSessionSchemaVersion = 2;
constexpr int kSubsystemStatusSchemaVersion = 3;
constexpr char kMigrationSql[] =
    "CREATE TABLE schema_migrations ("
    " version INTEGER PRIMARY KEY NOT NULL CHECK(version > 0),"
    " applied_at_utc TEXT NOT NULL CHECK(length(applied_at_utc) > 0)"
    ");"
    "INSERT INTO schema_migrations(version, applied_at_utc) "
    "VALUES (1, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));"
    "PRAGMA application_id=0x4453544E;"
    "PRAGMA user_version=1;";

constexpr char kSourceSessionMigrationSql[] =
    "CREATE TABLE source_registrations ("
    " peer_identity BLOB NOT NULL PRIMARY KEY"
    "   CHECK(length(peer_identity) BETWEEN 1 AND 128),"
    " station_id TEXT NOT NULL CHECK(length(station_id) BETWEEN 1 AND 128),"
    " source TEXT NOT NULL UNIQUE CHECK(length(source) BETWEEN 1 AND 128),"
    " registration_revision TEXT NOT NULL"
    "   CHECK(length(registration_revision) BETWEEN 1 AND 128),"
    " state INTEGER NOT NULL CHECK(state IN (1, 2, 3))"
    ") WITHOUT ROWID;"
    "CREATE TABLE source_cursors ("
    " source TEXT PRIMARY KEY NOT NULL"
    "   REFERENCES source_registrations(source)"
    "   ON UPDATE RESTRICT ON DELETE RESTRICT,"
    " source_instance TEXT NOT NULL CHECK(length(source_instance) BETWEEN 1 AND 128),"
    " last_sequence TEXT NULL"
    "   CHECK(last_sequence IS NULL OR length(last_sequence) BETWEEN 1 AND 20),"
    " updated_at_utc TEXT NOT NULL CHECK(length(updated_at_utc) > 0)"
    ") WITHOUT ROWID;"
    "INSERT INTO schema_migrations(version, applied_at_utc) "
    "VALUES (2, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));"
    "PRAGMA user_version=2;";

constexpr char kSubsystemStatusMigrationSql[] =
    "CREATE TABLE subsystem_registrations ("
    " station_id TEXT NOT NULL CHECK(length(station_id) BETWEEN 1 AND 128),"
    " subsystem_id TEXT NOT NULL CHECK(length(subsystem_id) BETWEEN 1 AND 128),"
    " source TEXT NOT NULL REFERENCES source_registrations(source)"
    "   ON UPDATE RESTRICT ON DELETE RESTRICT,"
    " registration_revision TEXT NOT NULL CHECK(length(registration_revision) BETWEEN 1 AND 128),"
    " state INTEGER NOT NULL CHECK(state IN (1, 2, 3)),"
    " PRIMARY KEY(station_id, subsystem_id),"
    " UNIQUE(station_id, subsystem_id, source)"
    ") WITHOUT ROWID;"
    "CREATE TRIGGER subsystem_registration_source_guard_insert "
    "BEFORE INSERT ON subsystem_registrations "
    "WHEN NOT EXISTS (SELECT 1 FROM source_registrations s "
    " WHERE s.station_id=NEW.station_id AND s.source=NEW.source AND s.state=1) "
    "BEGIN SELECT RAISE(ABORT, 'subsystem source must be active for this station'); END;"
    "CREATE TRIGGER subsystem_registration_source_guard_update "
    "BEFORE UPDATE OF station_id, source ON subsystem_registrations "
    "WHEN NOT EXISTS (SELECT 1 FROM source_registrations s "
    " WHERE s.station_id=NEW.station_id AND s.source=NEW.source AND s.state=1) "
    "BEGIN SELECT RAISE(ABORT, 'subsystem source must be active for this station'); END;"
    "CREATE TABLE accepted_events ("
    " event_id TEXT PRIMARY KEY NOT NULL,"
    " event_type TEXT NOT NULL,"
    " source TEXT NOT NULL,"
    " source_instance TEXT NULL,"
    " occurred_at TEXT NULL,"
    " received_at TEXT NOT NULL,"
    " source_sequence TEXT NULL CHECK(source_sequence IS NULL OR "
    "   (length(source_sequence) BETWEEN 1 AND 20 AND source_sequence NOT GLOB '*[^0-9]*')),"
    " operation_id TEXT NULL,"
    " correlation_id TEXT NULL,"
    " payload TEXT NULL,"
    " quality_status TEXT NOT NULL,"
    " schema_version TEXT NOT NULL,"
    " reason_code TEXT NULL,"
    " reason_text TEXT NULL"
    ") WITHOUT ROWID;"
    "CREATE INDEX accepted_events_source_cursor_idx "
    "ON accepted_events(source, source_instance, event_id);"
    "CREATE TABLE subsystem_status_history ("
    " event_id TEXT PRIMARY KEY NOT NULL REFERENCES accepted_events(event_id)"
    "   ON UPDATE RESTRICT ON DELETE RESTRICT,"
    " station_id TEXT NOT NULL,"
    " subsystem_id TEXT NOT NULL,"
    " source TEXT NOT NULL,"
    " status INTEGER NOT NULL CHECK(status IN (0, 1, 2, 3)),"
    " error_code TEXT NOT NULL,"
    " severity TEXT NOT NULL CHECK(severity IN ('INFO','WARNING','ERROR','CRITICAL')),"
    " description TEXT NOT NULL CHECK(length(description)>0),"
    " reason_text TEXT NULL,"
    " source_instance TEXT NOT NULL,"
    " source_sequence TEXT NOT NULL CHECK(length(source_sequence) BETWEEN 1 AND 20 "
    "   AND source_sequence NOT GLOB '*[^0-9]*'),"
    " committed_at_utc TEXT NOT NULL CHECK(length(committed_at_utc)>0),"
    " FOREIGN KEY(station_id, subsystem_id, source)"
    "   REFERENCES subsystem_registrations(station_id, subsystem_id, source)"
    "   ON UPDATE RESTRICT ON DELETE RESTRICT,"
    " UNIQUE(station_id, subsystem_id, event_id)"
    ") WITHOUT ROWID;"
    "CREATE INDEX subsystem_status_history_lookup_idx "
    "ON subsystem_status_history(station_id, subsystem_id, committed_at_utc);"
    "CREATE TABLE subsystem_status_current ("
    " station_id TEXT NOT NULL,"
    " subsystem_id TEXT NOT NULL,"
    " last_event_id TEXT NOT NULL,"
    " PRIMARY KEY(station_id, subsystem_id),"
    " FOREIGN KEY(station_id, subsystem_id)"
    "   REFERENCES subsystem_registrations(station_id, subsystem_id)"
    "   ON UPDATE RESTRICT ON DELETE RESTRICT,"
    " FOREIGN KEY(station_id, subsystem_id, last_event_id)"
    "   REFERENCES subsystem_status_history(station_id, subsystem_id, event_id)"
    "   ON UPDATE RESTRICT ON DELETE RESTRICT"
    ") WITHOUT ROWID;"
    "INSERT INTO schema_migrations(version, applied_at_utc) "
    "VALUES (3, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));"
    "PRAGMA user_version=3;";

[[nodiscard]] bool valid_database_path(std::string_view path) noexcept {
    if (path.size() < 2U || path.size() > kMaximumDatabasePathBytes ||
        path.front() != '/' || path.back() == '/' ||
        path.find(static_cast<char>(0)) != std::string_view::npos) {
        return false;
    }

    std::size_t offset = 1U;
    while (offset < path.size()) {
        const std::size_t separator = path.find('/', offset);
        const std::size_t end = separator == std::string_view::npos
            ? path.size() : separator;
        const std::string_view component = path.substr(offset, end - offset);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }
    return true;
}

[[nodiscard]] int execute_sql(sqlite3* database, const char* sql) noexcept {
    char* error_message = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &error_message);
    sqlite3_free(error_message);
    return status;
}

[[nodiscard]] bool query_int64(
    sqlite3* database, const char* sql, sqlite3_int64& output) noexcept {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bool valid = false;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        output = sqlite3_column_int64(statement, 0);
        valid = sqlite3_step(statement) == SQLITE_DONE;
    }
    const int finalize_status = sqlite3_finalize(statement);
    return valid && finalize_status == SQLITE_OK;
}

[[nodiscard]] bool query_text_equals(
    sqlite3* database, const char* sql, const char* expected) noexcept {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bool valid = false;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(statement, 0);
        valid = text != nullptr &&
                std::strcmp(reinterpret_cast<const char*>(text), expected) == 0 &&
                sqlite3_step(statement) == SQLITE_DONE;
    }
    const int finalize_status = sqlite3_finalize(statement);
    return valid && finalize_status == SQLITE_OK;
}

[[nodiscard]] bool configure_connection(
    sqlite3* database, std::int32_t busy_timeout_ms) noexcept {
    if (sqlite3_busy_timeout(database, busy_timeout_ms) != SQLITE_OK ||
        execute_sql(database, "PRAGMA foreign_keys=ON;") != SQLITE_OK ||
        execute_sql(database, "PRAGMA trusted_schema=OFF;") != SQLITE_OK ||
        execute_sql(database, "PRAGMA synchronous=EXTRA;") != SQLITE_OK ||
        execute_sql(database, "PRAGMA journal_mode=DELETE;") != SQLITE_OK) {
        return false;
    }

    sqlite3_int64 synchronous = 0;
    sqlite3_int64 foreign_keys = 0;
    sqlite3_int64 trusted_schema = 1;
    return query_text_equals(database, "PRAGMA journal_mode;", "delete") &&
           query_int64(database, "PRAGMA synchronous;", synchronous) &&
           synchronous == 3 &&
           query_int64(database, "PRAGMA foreign_keys;", foreign_keys) &&
           foreign_keys == 1 &&
           query_int64(database, "PRAGMA trusted_schema;", trusted_schema) &&
           trusted_schema == 0;
}

[[nodiscard]] bool has_user_schema_objects(sqlite3* database, sqlite3_int64& count) noexcept {
    return query_int64(
        database,
        "SELECT count(*) FROM sqlite_schema "
        "WHERE name NOT LIKE 'sqlite_%';",
        count);
}

[[nodiscard]] bool has_initial_migration(sqlite3* database) noexcept {
    sqlite3_int64 count = 0;
    return query_int64(
        database,
        "SELECT count(*) FROM schema_migrations "
        "WHERE version=1 AND length(applied_at_utc)>0;",
        count) && count == 1;
}

[[nodiscard]] bool has_current_migrations(sqlite3* database) noexcept {
    sqlite3_int64 count = 0;
    return query_int64(
        database,
        "SELECT count(*) FROM schema_migrations;",
        count) && count == kSubsystemStatusSchemaVersion &&
        has_initial_migration(database) &&
        query_int64(
            database,
            "SELECT count(*) FROM schema_migrations "
            "WHERE version=2 AND length(applied_at_utc)>0;",
            count) && count == 1 &&
        query_int64(
            database,
            "SELECT count(*) FROM schema_migrations "
            "WHERE version=3 AND length(applied_at_utc)>0;",
            count) && count == 1;
}

[[nodiscard]] bool apply_schema_transaction(
    sqlite3* database, const char* migration_sql) noexcept {
    if (execute_sql(database, "BEGIN EXCLUSIVE;") != SQLITE_OK) {
        return false;
    }

    const bool migrated = execute_sql(database, migration_sql) == SQLITE_OK;
    const int commit_status = migrated
        ? execute_sql(database, "COMMIT;")
        : SQLITE_ERROR;
    if (commit_status != SQLITE_OK) {
        (void)execute_sql(database, "ROLLBACK;");
        return false;
    }
    return true;
}

}  // namespace

SqliteDatabase::~SqliteDatabase() {
    if (!close() && database_ != nullptr) {
        (void)sqlite3_close_v2(database_);
        database_ = nullptr;
    }
}

SqliteDatabaseResult SqliteDatabase::open(
    const SqliteDatabaseConfig& config) noexcept {
    if (!close()) {
        return {SqliteDatabaseStatus::CloseFailed, last_sqlite_status_, 0};
    }
    last_sqlite_status_ = SQLITE_OK;

    if (!valid_database_path(config.database_path)) {
        return {SqliteDatabaseStatus::InvalidPath, SQLITE_MISUSE, 0};
    }
    if (config.busy_timeout_ms < 0 || config.busy_timeout_ms > 60000) {
        return {SqliteDatabaseStatus::InvalidBusyTimeout, SQLITE_MISUSE, 0};
    }

    std::array<char, kMaximumDatabasePathBytes + 1U> path{};
    std::copy(config.database_path.begin(), config.database_path.end(), path.begin());

    const int open_status = sqlite3_open_v2(
        path.data(), &database_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
            SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
        nullptr);
    if (open_status != SQLITE_OK || database_ == nullptr) {
        last_sqlite_status_ = database_ == nullptr
            ? open_status : sqlite3_extended_errcode(database_);
        (void)close();
        return {SqliteDatabaseStatus::OpenFailed, last_sqlite_status_, 0};
    }

    (void)sqlite3_extended_result_codes(database_, 1);
    if (sqlite3_busy_timeout(database_, config.busy_timeout_ms) != SQLITE_OK) {
        last_sqlite_status_ = sqlite3_extended_errcode(database_);
        (void)close();
        return {SqliteDatabaseStatus::SettingsFailed, last_sqlite_status_, 0};
    }

    sqlite3_int64 application_id = 0;
    sqlite3_int64 stored_version = 0;
    if (!query_int64(database_, "PRAGMA application_id;", application_id) ||
        !query_int64(database_, "PRAGMA user_version;", stored_version)) {
        last_sqlite_status_ = sqlite3_extended_errcode(database_);
        (void)close();
        return {SqliteDatabaseStatus::SchemaInvalid, last_sqlite_status_, 0};
    }

    if (application_id != 0 && application_id != kDockStationApplicationId) {
        last_sqlite_status_ = SQLITE_NOTADB;
        (void)close();
        return {SqliteDatabaseStatus::ApplicationIdMismatch, last_sqlite_status_, 0};
    }
    if (stored_version < 0 ||
        stored_version > static_cast<sqlite3_int64>(kCurrentSchemaVersion)) {
        last_sqlite_status_ = SQLITE_SCHEMA;
        (void)close();
        return {SqliteDatabaseStatus::SchemaVersionUnsupported,
                last_sqlite_status_, 0};
    }

    const bool uninitialized_database =
        application_id == 0 && stored_version == 0;
    const bool incomplete_database =
        application_id == kDockStationApplicationId && stored_version == 0;
    if (application_id == 0 && stored_version != 0) {
        last_sqlite_status_ = SQLITE_SCHEMA;
        (void)close();
        return {SqliteDatabaseStatus::ApplicationIdMismatch,
                last_sqlite_status_, 0};
    }
    if (uninitialized_database || incomplete_database) {
        sqlite3_int64 existing_objects = 0;
        if (!has_user_schema_objects(database_, existing_objects) ||
            existing_objects != 0) {
            last_sqlite_status_ = SQLITE_SCHEMA;
            (void)close();
            return {SqliteDatabaseStatus::ApplicationIdMismatch,
                    last_sqlite_status_, 0};
        }
    }

    // Validate the file identity/version before changing journal or connection settings.
    // This prevents an unrelated SQLite database from being silently converted out of WAL.
    if (!configure_connection(database_, config.busy_timeout_ms)) {
        last_sqlite_status_ = sqlite3_extended_errcode(database_);
        (void)close();
        return {SqliteDatabaseStatus::SettingsFailed, last_sqlite_status_, 0};
    }

    if (uninitialized_database || incomplete_database) {
        if (!apply_schema_transaction(database_, kMigrationSql)) {
            last_sqlite_status_ = sqlite3_extended_errcode(database_);
            (void)close();
            return {SqliteDatabaseStatus::SchemaMigrationFailed,
                    last_sqlite_status_, 0};
        }
        application_id = kDockStationApplicationId;
        stored_version = kInitialSchemaVersion;
    }

    if (stored_version == kInitialSchemaVersion &&
        !has_initial_migration(database_)) {
        last_sqlite_status_ = SQLITE_SCHEMA;
        (void)close();
        return {SqliteDatabaseStatus::SchemaInvalid, last_sqlite_status_, 0};
    }

    while (stored_version < kCurrentSchemaVersion) {
        bool migrated = false;
        if (stored_version + 1 ==
            static_cast<sqlite3_int64>(kSourceSessionSchemaVersion)) {
            migrated = apply_schema_transaction(database_, kSourceSessionMigrationSql);
        } else if (stored_version + 1 ==
                   static_cast<sqlite3_int64>(kSubsystemStatusSchemaVersion)) {
            migrated = apply_schema_transaction(database_, kSubsystemStatusMigrationSql);
        }
        if (!migrated) {
            last_sqlite_status_ = sqlite3_extended_errcode(database_);
            (void)close();
            return {SqliteDatabaseStatus::SchemaMigrationFailed,
                    last_sqlite_status_, 0};
        }
        ++stored_version;
    }

    if (application_id != kDockStationApplicationId ||
        stored_version != kCurrentSchemaVersion ||
        !has_current_migrations(database_)) {
        last_sqlite_status_ = SQLITE_SCHEMA;
        (void)close();
        return {SqliteDatabaseStatus::SchemaInvalid, last_sqlite_status_, 0};
    }

    schema_version_ = static_cast<int>(stored_version);
    last_sqlite_status_ = SQLITE_OK;
    return {SqliteDatabaseStatus::Ready, SQLITE_OK, schema_version_};
}

bool SqliteDatabase::close() noexcept {
    if (database_ == nullptr) {
        schema_version_ = 0;
        return true;
    }

    const int status = sqlite3_close(database_);
    if (status != SQLITE_OK) {
        // sqlite3_close() returns SQLITE_BUSY while statements remain alive.
        // Preserve that exact result; querying a separate extended error code
        // can report SQLITE_OK even though the close operation was refused.
        last_sqlite_status_ = status;
        return false;
    }

    database_ = nullptr;
    schema_version_ = 0;
    return true;
}

bool SqliteDatabase::is_open() const noexcept {
    return database_ != nullptr;
}

int SqliteDatabase::schema_version() const noexcept {
    return schema_version_;
}

int SqliteDatabase::last_sqlite_status() const noexcept {
    return last_sqlite_status_;
}

sqlite3* SqliteDatabase::native_handle() noexcept {
    return database_;
}

}  // namespace dockstation::platform::persistence
