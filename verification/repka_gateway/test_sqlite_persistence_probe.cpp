#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include <sqlite3.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/persistence/sqlite_database.hpp"

using dockstation::platform::persistence::SqliteDatabase;
using dockstation::platform::persistence::SqliteDatabaseConfig;
using dockstation::platform::persistence::SqliteDatabaseStatus;

namespace {

int checks = 0;
int failures = 0;

void expect(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << description << '\n';
    } else {
        ++failures;
        std::cerr << "FAIL - " << description << '\n';
    }
}

bool execute(sqlite3* database, const char* sql, int expected_status = SQLITE_OK) {
    char* error_message = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &error_message);
    if (status != expected_status) {
        std::cerr << "SQLite status " << status
                  << " (expected " << expected_status << ')';
        if (error_message != nullptr) {
            std::cerr << ": " << error_message;
        }
        std::cerr << '\n';
    }
    sqlite3_free(error_message);
    return status == expected_status;
}

bool query_integer(sqlite3* database, const char* sql, sqlite3_int64& value) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bool success = false;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int64(statement, 0);
        success = sqlite3_step(statement) == SQLITE_DONE;
    }
    const int finalize_status = sqlite3_finalize(statement);
    return success && finalize_status == SQLITE_OK;
}

bool query_text(sqlite3* database, const char* sql, std::string& value) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bool success = false;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* result = sqlite3_column_text(statement, 0);
        if (result != nullptr) {
            value.assign(reinterpret_cast<const char*>(result));
            success = sqlite3_step(statement) == SQLITE_DONE;
        }
    }
    const int finalize_status = sqlite3_finalize(statement);
    return success && finalize_status == SQLITE_OK;
}

bool open_database(SqliteDatabase& storage, const std::string& path) {
    const auto result = storage.open(SqliteDatabaseConfig{path, 50});
    if (!result) {
        std::cerr << "SqliteDatabase open failed: status="
                  << static_cast<int>(result.status)
                  << ", sqlite_status=" << result.sqlite_status << '\n';
    }
    return static_cast<bool>(result);
}

bool create_probe_schema(sqlite3* database) {
    return execute(database,
        "CREATE TABLE probe_parent ("
        " id INTEGER PRIMARY KEY,"
        " label TEXT NOT NULL"
        ");"
        "CREATE TABLE probe_child ("
        " id INTEGER PRIMARY KEY,"
        " parent_id INTEGER NOT NULL REFERENCES probe_parent(id),"
        " label TEXT NOT NULL"
        ");");
}

bool count_rows(sqlite3* database, const char* table, sqlite3_int64& count) {
    if (table == nullptr) {
        return false;
    }
    if (std::string(table) == "probe_parent") {
        return query_integer(database, "SELECT count(*) FROM probe_parent;", count);
    }
    if (std::string(table) == "probe_child") {
        return query_integer(database, "SELECT count(*) FROM probe_child;", count);
    }
    return false;
}

bool run_uncommitted_child_transaction(const std::string& path) {
    const pid_t child = ::fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        SqliteDatabase storage{};
        if (!open_database(storage, path)) {
            ::_exit(101);
        }
        sqlite3* database = storage.native_handle();
        if (database == nullptr ||
            !execute(database, "BEGIN IMMEDIATE;") ||
            !execute(database,
                "INSERT INTO probe_parent(id, label) VALUES(3, 'uncommitted');") ||
            !execute(database,
                "INSERT INTO probe_child(id, parent_id, label) "
                "VALUES(3, 3, 'uncommitted child');")) {
            ::_exit(102);
        }

        // Deliberately bypass close() and COMMIT to model abrupt process death.
        ::_exit(0);
    }

    int child_status = 0;
    if (::waitpid(child, &child_status, 0) != child) {
        return false;
    }
    return WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;
}

void remove_database_files(const std::string& path, const std::string& directory) {
    (void)std::remove((path + "-journal").c_str());
    (void)std::remove(path.c_str());
    (void)::rmdir(directory.c_str());
}

}  // namespace

int main() {
    char directory_template[] = "/tmp/dockstation_sqlite_probe_XXXXXX";
    char* created_directory = ::mkdtemp(directory_template);
    if (created_directory == nullptr) {
        std::cerr << "Could not create a temporary SQLite test directory\n";
        return EXIT_FAILURE;
    }

    const std::string directory{created_directory};
    const std::string path = directory + "/station.sqlite3";

    SqliteDatabase invalid_path_storage{};
    const auto invalid_path = invalid_path_storage.open(
        SqliteDatabaseConfig{"relative/station.sqlite3", 50});
    expect(invalid_path.status == SqliteDatabaseStatus::InvalidPath,
           "rejects relative database paths before opening a file");

    SqliteDatabase storage{};
    const bool opened = open_database(storage, path);
    expect(opened && storage.schema_version() == SqliteDatabase::kCurrentSchemaVersion,
           "opens an on-disk database and applies the initial schema migration");
    if (!opened) {
        remove_database_files(path, directory);
        return EXIT_FAILURE;
    }

    sqlite3* database = storage.native_handle();
    std::string journal_mode;
    sqlite3_int64 synchronous = 0;
    sqlite3_int64 foreign_keys = 0;
    sqlite3_int64 trusted_schema = 1;
    sqlite3_int64 application_id = 0;
    sqlite3_int64 user_version = 0;
    sqlite3_int64 migration_count = 0;
    const bool settings_verified =
        query_text(database, "PRAGMA journal_mode;", journal_mode) &&
        query_integer(database, "PRAGMA synchronous;", synchronous) &&
        query_integer(database, "PRAGMA foreign_keys;", foreign_keys) &&
        query_integer(database, "PRAGMA trusted_schema;", trusted_schema) &&
        query_integer(database, "PRAGMA application_id;", application_id) &&
        query_integer(database, "PRAGMA user_version;", user_version) &&
        query_integer(database,
            "SELECT count(*) FROM schema_migrations WHERE version=1;",
            migration_count);
    expect(settings_verified && journal_mode == "delete" &&
               synchronous == 3 && foreign_keys == 1 && trusted_schema == 0 &&
               application_id == 0x4453544E && user_version == 1 &&
               migration_count == 1,
           "verifies DELETE/EXTRA, foreign keys, trusted schema and database identity/version");

    const bool schema_created = settings_verified && create_probe_schema(database);
    expect(schema_created, "creates two related tables for atomic transaction testing");

    bool rolled_back = false;
    if (schema_created) {
        rolled_back =
            execute(database, "BEGIN IMMEDIATE;") &&
            execute(database,
                "INSERT INTO probe_parent(id, label) VALUES(1, 'rollback');") &&
            execute(database,
                "INSERT INTO probe_child(id, parent_id, label) "
                "VALUES(1, 1, 'rollback child');") &&
            execute(database, "ROLLBACK;");
    }
    sqlite3_int64 parent_count = -1;
    sqlite3_int64 child_count = -1;
    rolled_back = rolled_back &&
                  count_rows(database, "probe_parent", parent_count) &&
                  count_rows(database, "probe_child", child_count) &&
                  parent_count == 0 && child_count == 0;
    expect(rolled_back, "ROLLBACK leaves both related tables unchanged");

    const bool committed =
        execute(database, "BEGIN IMMEDIATE;") &&
        execute(database,
            "INSERT INTO probe_parent(id, label) VALUES(2, 'committed');") &&
        execute(database,
            "INSERT INTO probe_child(id, parent_id, label) "
            "VALUES(2, 2, 'committed child');") &&
        execute(database, "COMMIT;");
    expect(committed, "COMMIT atomically stores related rows");

    storage.close();
    expect(!storage.is_open(), "closes the database connection");
    expect(open_database(storage, path),
           "reopens the same database through the production adapter");
    database = storage.native_handle();

    parent_count = -1;
    child_count = -1;
    const bool persisted =
        database != nullptr &&
        count_rows(database, "probe_parent", parent_count) &&
        count_rows(database, "probe_child", child_count) &&
        parent_count == 1 && child_count == 1;
    expect(persisted, "committed rows remain available after close and reopen");

    storage.close();
    const bool child_exited = run_uncommitted_child_transaction(path);
    expect(child_exited, "child process exits after writing but before COMMIT");

    expect(open_database(storage, path),
           "reopens and recovers the database after abrupt process exit");
    database = storage.native_handle();

    parent_count = -1;
    child_count = -1;
    std::string integrity;
    const bool crash_rollback =
        database != nullptr &&
        count_rows(database, "probe_parent", parent_count) &&
        count_rows(database, "probe_child", child_count) &&
        parent_count == 1 && child_count == 1 &&
        query_text(database, "PRAGMA integrity_check;", integrity) &&
        integrity == "ok";
    expect(crash_rollback,
           "uncommitted rows are absent and integrity_check reports ok after process crash");

    SqliteDatabase competing_storage{};
    const bool second_opened = open_database(competing_storage, path);
    const bool first_writer_locked =
        database != nullptr && execute(database, "BEGIN IMMEDIATE;");
    const bool competing_writer_blocked =
        second_opened && first_writer_locked &&
        execute(competing_storage.native_handle(),
                "BEGIN IMMEDIATE;", SQLITE_BUSY);
    if (first_writer_locked) {
        (void)execute(database, "ROLLBACK;");
    }
    expect(second_opened && first_writer_locked && competing_writer_blocked,
           "serializes competing writers instead of allowing overlapping write transactions");

    competing_storage.close();
    storage.close();
    remove_database_files(path, directory);

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
