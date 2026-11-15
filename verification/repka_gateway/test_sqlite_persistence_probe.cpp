#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include <sqlite3.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

bool query_integer(sqlite3* database, const char* sql, int& value) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bool success = false;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int(statement, 0);
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

sqlite3* open_database(const std::string& path) {
    sqlite3* database = nullptr;
    const int status = sqlite3_open_v2(
        path.c_str(), &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (status != SQLITE_OK) {
        if (database != nullptr) {
            sqlite3_close(database);
        }
        return nullptr;
    }

    (void)sqlite3_extended_result_codes(database, 0);
    if (sqlite3_busy_timeout(database, 50) != SQLITE_OK) {
        sqlite3_close(database);
        return nullptr;
    }
    return database;
}

bool configure_database(sqlite3* database) {
    if (database == nullptr ||
        !execute(database, "PRAGMA journal_mode=DELETE;") ||
        !execute(database, "PRAGMA synchronous=EXTRA;") ||
        !execute(database, "PRAGMA foreign_keys=ON;")) {
        return false;
    }

    std::string journal_mode;
    int synchronous = 0;
    int foreign_keys = 0;
    return query_text(database, "PRAGMA journal_mode;", journal_mode) &&
           query_integer(database, "PRAGMA synchronous;", synchronous) &&
           query_integer(database, "PRAGMA foreign_keys;", foreign_keys) &&
           journal_mode == "delete" &&
           synchronous == 3 &&
           foreign_keys == 1;
}

bool create_schema(sqlite3* database) {
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

bool count_rows(sqlite3* database, const char* table, int& count) {
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
        sqlite3* database = open_database(path);
        if (database == nullptr || !configure_database(database) ||
            !execute(database, "BEGIN IMMEDIATE;") ||
            !execute(database,
                "INSERT INTO probe_parent(id, label) VALUES(3, 'uncommitted');") ||
            !execute(database,
                "INSERT INTO probe_child(id, parent_id, label) "
                "VALUES(3, 3, 'uncommitted child');")) {
            ::_exit(101);
        }

        // Deliberately bypass sqlite3_close() and COMMIT to model abrupt process death.
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
    expect(sqlite3_libversion_number() > 0, "SQLite library is present");
    std::cout << "SQLite runtime version: " << sqlite3_libversion() << '\n';

    sqlite3* database = open_database(path);
    expect(database != nullptr, "opens a real on-disk SQLite database");
    if (database == nullptr) {
        remove_database_files(path, directory);
        return EXIT_FAILURE;
    }

    const bool configured = configure_database(database);
    expect(configured,
           "configures and verifies DELETE journal, synchronous EXTRA and foreign keys");

    const bool schema_created = configured && create_schema(database);
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
    int parent_count = -1;
    int child_count = -1;
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

    const int close_status = sqlite3_close(database);
    expect(close_status == SQLITE_OK, "closes database cleanly after statements are finalized");

    database = open_database(path);
    const bool reopened = database != nullptr && configure_database(database);
    expect(reopened, "reopens the same on-disk database with the required durability profile");

    parent_count = -1;
    child_count = -1;
    const bool persisted = reopened &&
        count_rows(database, "probe_parent", parent_count) &&
        count_rows(database, "probe_child", child_count) &&
        parent_count == 1 && child_count == 1;
    expect(persisted, "committed rows remain available after close and reopen");

    if (database != nullptr) {
        expect(sqlite3_close(database) == SQLITE_OK,
               "closes the database before abrupt-process recovery test");
        database = nullptr;
    }

    const bool child_exited = run_uncommitted_child_transaction(path);
    expect(child_exited, "child process exits after writing but before COMMIT");

    database = open_database(path);
    const bool crash_reopened = database != nullptr && configure_database(database);
    expect(crash_reopened, "reopens and recovers the database after abrupt process exit");

    parent_count = -1;
    child_count = -1;
    std::string integrity;
    const bool crash_rollback = crash_reopened &&
        count_rows(database, "probe_parent", parent_count) &&
        count_rows(database, "probe_child", child_count) &&
        parent_count == 1 && child_count == 1 &&
        query_text(database, "PRAGMA integrity_check;", integrity) &&
        integrity == "ok";
    expect(crash_rollback,
           "uncommitted rows are absent and integrity_check reports ok after process crash");

    if (database != nullptr) {
        sqlite3* competing_writer = open_database(path);
        const bool second_opened =
            competing_writer != nullptr && configure_database(competing_writer);
        const bool first_writer_locked =
            execute(database, "BEGIN IMMEDIATE;");
        const bool competing_writer_blocked =
            second_opened &&
            !first_writer_locked ? false :
            execute(competing_writer, "BEGIN IMMEDIATE;", SQLITE_BUSY);
        if (first_writer_locked) {
            (void)execute(database, "ROLLBACK;");
        }
        if (competing_writer != nullptr) {
            (void)sqlite3_close(competing_writer);
        }
        expect(second_opened && first_writer_locked && competing_writer_blocked,
               "serializes competing writers instead of allowing overlapping write transactions");
        (void)sqlite3_close(database);
        database = nullptr;
    } else {
        expect(false, "serializes competing writers instead of allowing overlapping write transactions");
    }

    remove_database_files(path, directory);
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
