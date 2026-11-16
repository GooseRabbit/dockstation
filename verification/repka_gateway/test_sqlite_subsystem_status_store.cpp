#include "core/source_session_coordinator.hpp"
#include "core/source_session_verifier.hpp"
#include "platform/persistence/sqlite_source_session_store.hpp"
#include "platform/persistence/sqlite_subsystem_status_store.hpp"
#include "../regression/source_session_test_support.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <sqlite3.h>
#include <unistd.h>

using dockstation::core::Event;
using dockstation::core::RegistrySourceSessionVerifier;
using dockstation::core::ReportedSubsystemStatus;
using dockstation::core::SourceRegistrationState;
using dockstation::core::SourceSessionCoordinator;
using dockstation::core::SourceSessionCoordinatorStatus;
using dockstation::core::SubsystemRegistrationState;
using dockstation::core::SubsystemStatusCommitStatus;
using dockstation::platform::persistence::SqliteDatabase;
using dockstation::platform::persistence::SqliteDatabaseConfig;
using dockstation::platform::persistence::SqliteSourceRegistrationWriteStatus;
using dockstation::platform::persistence::SqliteSourceSessionStore;
using dockstation::platform::persistence::SqliteSubsystemRegistrationWriteStatus;
using dockstation::platform::persistence::SqliteSubsystemStatusStore;
using dockstation::verification::SourceSessionEvidenceTestAccess;

namespace {
int checks = 0;
int failures = 0;
constexpr std::string_view kStation{"station-1"};
constexpr std::string_view kSource{"esp32.main"};
constexpr std::string_view kSubsystem{"weather.temperature"};
constexpr std::string_view kPeerIdentity{"peer\0identity", 13U};
constexpr std::string_view kProof{"0123456789abcdef0123456789abcdef"};

void expect(bool condition, const char* description) {
    ++checks;
    if (condition) std::cout << "PASS - " << description << '\n';
    else { ++failures; std::cerr << "FAIL - " << description << '\n'; }
}

bool execute(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::cerr << "SQLite status " << rc;
        if (error != nullptr) std::cerr << ": " << error;
        std::cerr << '\n';
    }
    sqlite3_free(error);
    return rc == SQLITE_OK;
}

bool query_integer(sqlite3* db, const char* sql, sqlite3_int64& value) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    bool valid = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        value = sqlite3_column_int64(st, 0);
        valid = sqlite3_step(st) == SQLITE_DONE;
    }
    const int final = sqlite3_finalize(st);
    return valid && final == SQLITE_OK;
}

void remove_database_files(const std::string& path) {
    (void)std::remove((path + "-journal").c_str());
    (void)std::remove((path + "-wal").c_str());
    (void)std::remove((path + "-shm").c_str());
    (void)std::remove(path.c_str());
}

dockstation::core::SourceSessionEvidence make_evidence() {
    return SourceSessionEvidenceTestAccess::make(
        kSource, "boot-1", kPeerIdentity, kProof);
}

Event make_session_event() {
    Event event{};
    event.source = kSource;
    event.source_instance = "boot-1";
    return event;
}

Event make_event(std::string_view id, std::uint64_t sequence,
                 std::string_view payload) {
    Event event{};
    event.event_id = id;
    event.event_type = "SUBSYSTEM_STATUS_UPDATED";
    event.source = kSource;
    event.source_instance = "boot-1";
    event.received_at = "2026-11-16T10:00:00.000Z";
    event.source_sequence = sequence;
    event.payload = payload;
    event.quality_status = "VALID";
    event.schema_version = "1";
    return event;
}

dockstation::core::DecodedSubsystemStatus make_report(
    ReportedSubsystemStatus status, std::string_view description,
    std::string_view error_code = "OK", std::string_view severity = "INFO") {
    return {kSubsystem, status, error_code, severity, description, std::nullopt};
}

bool current_event_is(sqlite3* db, std::string_view expected) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT last_event_id FROM subsystem_status_current "
        "WHERE station_id='station-1' AND subsystem_id='weather.temperature';",
        -1, &st, nullptr) != SQLITE_OK) return false;
    bool valid = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const auto* bytes = sqlite3_column_text(st, 0);
        const int length = sqlite3_column_bytes(st, 0);
        valid = bytes != nullptr &&
            std::string_view{reinterpret_cast<const char*>(bytes),
                             static_cast<std::size_t>(length)} == expected;
        valid = valid && sqlite3_step(st) == SQLITE_DONE;
    }
    const int final = sqlite3_finalize(st);
    return valid && final == SQLITE_OK;
}

}  // namespace

int main() {
    char directory_template[] = "/tmp/dockstation_subsystem_status_XXXXXX";
    char* directory = ::mkdtemp(directory_template);
    if (directory == nullptr) return EXIT_FAILURE;
    const std::string path = std::string{directory} + "/station.sqlite3";

    SqliteDatabase database{};
    const auto opened = database.open(SqliteDatabaseConfig{path, 100});
    expect(static_cast<bool>(opened) && opened.schema_version == 3,
           "fresh database applies schema migrations 1, 2 and 3");
    if (!opened) {
        remove_database_files(path);
        (void)::rmdir(directory);
        return EXIT_FAILURE;
    }

    sqlite3_int64 versions = 0;
    sqlite3_int64 tables = 0;
    expect(query_integer(database.native_handle(),
        "SELECT count(*) FROM schema_migrations WHERE version IN (1,2,3);",
        versions) && versions == 3 &&
        query_integer(database.native_handle(),
        "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name IN "
        "('subsystem_registrations','accepted_events','subsystem_status_history',"
        "'subsystem_status_current');", tables) && tables == 4,
        "schema v3 migration ledger and domain tables agree");

    SqliteSourceSessionStore source_store{database};
    expect(source_store.create_source_registration(
        kStation, kSource, "source-r1", kPeerIdentity).status ==
        SqliteSourceRegistrationWriteStatus::Created,
        "creates ACTIVE source registration");

    RegistrySourceSessionVerifier verifier{source_store, kStation};
    SourceSessionCoordinator coordinator{verifier, source_store, source_store};
    const auto session = coordinator.confirm(make_session_event(), make_evidence());
    expect(session.status == SourceSessionCoordinatorStatus::Confirmed &&
           session.cursor.source_instance == std::optional<std::string_view>{"boot-1"} &&
           !session.cursor.last_sequence,
           "confirms source session and initializes an empty sequence cursor");

    SqliteSubsystemStatusStore store{database};
    expect(store.create_subsystem_registration(
        kStation, kSubsystem, kSource, "sub-r1").status ==
        SqliteSubsystemRegistrationWriteStatus::Created,
        "creates subsystem registration under matching ACTIVE source");
    expect(store.create_subsystem_registration(
        kStation, kSubsystem, kSource, "sub-r2").status ==
        SqliteSubsystemRegistrationWriteStatus::Conflict,
        "rejects duplicate subsystem registration");
    expect(store.create_subsystem_registration(
        "station-2", "lid.motor", kSource, "sub-r1").status ==
        SqliteSubsystemRegistrationWriteStatus::SourceNotRegistered,
        "source registration from another Station ID cannot provision a subsystem");

    const auto registration = store.find_subsystem(kStation, kSubsystem);
    expect(registration.status ==
        dockstation::core::SubsystemRegistrationLookupStatus::Found &&
        registration.record.station_id == kStation &&
        registration.record.subsystem_id == kSubsystem &&
        registration.record.source == kSource &&
        registration.record.registration_revision == "sub-r1" &&
        registration.record.state == SubsystemRegistrationState::Active,
        "looks up the exact ACTIVE subsystem registration");
    expect(store.find_by_id("event-1").status ==
        dockstation::core::EventRepositoryLookupStatus::NotFound,
        "missing event ID is distinct from repository error");

    const std::string payload1 =
        R"({"subsystem_id":"weather.temperature","status":"PASS","error_code":"OK","severity":"INFO","description":"Temperature responding"})";
    const Event event1 = make_event("event-1", 1U, payload1);
    const auto report1 = make_report(
        ReportedSubsystemStatus::Pass, "Temperature responding");
    const dockstation::core::SourceCursor cursor0{kSource, "boot-1", std::nullopt};
    const auto first = store.commit_subsystem_status(
        registration.record, report1, event1, cursor0);
    expect(first.status == SubsystemStatusCommitStatus::Committed,
        "first report commits event, history, current pointer and cursor atomically");

    sqlite3_int64 history_count = 0;
    const auto stored_event = store.find_by_id("event-1");
    expect(stored_event.status == dockstation::core::EventRepositoryLookupStatus::Found &&
        stored_event.event.event_type == "SUBSYSTEM_STATUS_UPDATED" &&
        stored_event.event.source_sequence == std::optional<std::uint64_t>{1U} &&
        stored_event.event.payload == std::optional<std::string_view>{payload1} &&
        query_integer(database.native_handle(),
            "SELECT count(*) FROM subsystem_status_history;", history_count) &&
        history_count == 1 && current_event_is(database.native_handle(), "event-1"),
        "stores original Event, decoded status history and current pointer");

    expect(store.commit_subsystem_status(
        registration.record, report1, event1, cursor0).status ==
        SubsystemStatusCommitStatus::DuplicateEvent,
        "same event ID and content is detected as duplicate");

    const std::string changed_payload =
        R"({"subsystem_id":"weather.temperature","status":"FAIL","error_code":"SENSOR_FAILED","severity":"ERROR","description":"Changed content"})";
    const Event conflicting_event = make_event("event-1", 1U, changed_payload);
    expect(store.commit_subsystem_status(
        registration.record,
        make_report(ReportedSubsystemStatus::Fail, "Changed content",
                    "SENSOR_FAILED", "ERROR"),
        conflicting_event, cursor0).status == SubsystemStatusCommitStatus::EventConflict,
        "same event ID with changed content is rejected as conflict");

    const std::string payload2 =
        R"({"subsystem_id":"weather.temperature","status":"DEGRADED","error_code":"HUMIDITY_WARNING","severity":"WARNING","description":"Humidity degraded"})";
    const Event event2 = make_event("event-2", 2U, payload2);
    const auto second = store.commit_subsystem_status(
        registration.record,
        make_report(ReportedSubsystemStatus::Degraded, "Humidity degraded",
                    "HUMIDITY_WARNING", "WARNING"),
        event2, {kSource, "boot-1", std::optional<std::uint64_t>{1U}});
    expect(second.status == SubsystemStatusCommitStatus::Committed &&
        query_integer(database.native_handle(),
            "SELECT count(*) FROM subsystem_status_history;", history_count) &&
        history_count == 2 && current_event_is(database.native_handle(), "event-2"),
        "newer report appends history and updates current pointer");

    const Event event3 = make_event(
        "event-3", 3U,
        R"({"subsystem_id":"weather.temperature","status":"PASS","error_code":"OK","severity":"INFO","description":"Recovered"})");
    expect(store.commit_subsystem_status(
        registration.record, make_report(ReportedSubsystemStatus::Pass, "Recovered"),
        event3, {kSource, "boot-1", std::optional<std::uint64_t>{1U}}).status ==
            SubsystemStatusCommitStatus::SourceCursorChanged &&
        store.find_by_id("event-3").status ==
            dockstation::core::EventRepositoryLookupStatus::NotFound &&
        query_integer(database.native_handle(),
            "SELECT count(*) FROM subsystem_status_history;", history_count) &&
        history_count == 2 && current_event_is(database.native_handle(), "event-2"),
        "stale cursor causes no partial event/history/current writes");

    const auto disabled = store.change_subsystem_state(
        kStation, kSubsystem, "sub-r1", "sub-r2", SubsystemRegistrationState::Disabled);
    expect(disabled.status == SqliteSubsystemRegistrationWriteStatus::Updated &&
        store.commit_subsystem_status(
            registration.record, make_report(ReportedSubsystemStatus::Pass, "Must reject"),
            event3, {kSource, "boot-1", std::optional<std::uint64_t>{2U}}).status ==
            SubsystemStatusCommitStatus::RegistrationChanged &&
        store.find_by_id("event-3").status ==
            dockstation::core::EventRepositoryLookupStatus::NotFound,
        "commit rechecks ACTIVE subsystem registration and its revision");

    const auto reactivated = store.change_subsystem_state(
        kStation, kSubsystem, "sub-r2", "sub-r3", SubsystemRegistrationState::Active);
    const auto latest_registration = store.find_subsystem(kStation, kSubsystem);
    expect(reactivated.status == SqliteSubsystemRegistrationWriteStatus::Updated &&
        latest_registration.status ==
            dockstation::core::SubsystemRegistrationLookupStatus::Found,
        "reactivation requires a new registration revision");

    expect(execute(database.native_handle(),
        "CREATE TRIGGER force_status_history_failure "
        "BEFORE INSERT ON subsystem_status_history "
        "BEGIN SELECT RAISE(ABORT,'forced rollback'); END;"),
        "adds deterministic failure after accepted Event insert");
    sqlite3_int64 event3_count = -1;
    expect(store.commit_subsystem_status(
        latest_registration.record,
        make_report(ReportedSubsystemStatus::Pass, "Rollback test"),
        event3, {kSource, "boot-1", std::optional<std::uint64_t>{2U}}).status ==
            SubsystemStatusCommitStatus::Error &&
        query_integer(database.native_handle(),
            "SELECT count(*) FROM accepted_events WHERE event_id='event-3';",
            event3_count) && event3_count == 0 &&
        query_integer(database.native_handle(),
            "SELECT count(*) FROM subsystem_status_history;", history_count) &&
        history_count == 2 && current_event_is(database.native_handle(), "event-2"),
        "forced mid-transaction failure rolls back Event and history/current/cursor");
    expect(execute(database.native_handle(),"DROP TRIGGER force_status_history_failure;"),
        "removes forced failure trigger");

    expect(store.commit_subsystem_status(
        latest_registration.record,
        make_report(ReportedSubsystemStatus::Pass, "Recovered"),
        event3, {kSource, "boot-1", std::optional<std::uint64_t>{2U}}).status ==
            SubsystemStatusCommitStatus::Committed &&
        current_event_is(database.native_handle(), "event-3"),
        "retry after rollback commits one complete status report");

    const auto source_disabled = source_store.change_source_state(
        kSource, "source-r1", "source-r2", SourceRegistrationState::Disabled);
    const Event event4 = make_event(
        "event-4", 4U,
        R"({"subsystem_id":"weather.temperature","status":"PASS","error_code":"OK","severity":"INFO","description":"Source disabled"})");
    expect(source_disabled.status == SqliteSourceRegistrationWriteStatus::Updated &&
        store.commit_subsystem_status(
            latest_registration.record,
            make_report(ReportedSubsystemStatus::Pass, "Source disabled"),
            event4, {kSource, "boot-1", std::optional<std::uint64_t>{3U}}).status ==
            SubsystemStatusCommitStatus::RegistrationChanged,
        "commit rechecks transport-source ACTIVE registration in the same Station ID");

    const auto revoked = store.change_subsystem_state(
        kStation, kSubsystem, "sub-r3", "sub-r4", SubsystemRegistrationState::Revoked);
    const auto reactivate_revoked = store.change_subsystem_state(
        kStation, kSubsystem, "sub-r4", "sub-r5", SubsystemRegistrationState::Active);
    expect(revoked.status == SqliteSubsystemRegistrationWriteStatus::Updated &&
        reactivate_revoked.status == SqliteSubsystemRegistrationWriteStatus::RevisionMismatch,
        "REVOKED subsystem registration is terminal");

    expect(database.close(), "closes SQLite database");
    const auto reopened = database.open(SqliteDatabaseConfig{path, 100});
    expect(static_cast<bool>(reopened) && reopened.schema_version == 3 &&
        query_integer(database.native_handle(),
            "SELECT count(*) FROM subsystem_status_history;", history_count) &&
        history_count == 3 && current_event_is(database.native_handle(), "event-3"),
        "status history and current pointer persist across close/reopen");
    expect(database.close(), "closes reopened database");

    remove_database_files(path);
    (void)::rmdir(directory);
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
