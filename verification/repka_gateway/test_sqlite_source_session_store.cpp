#include "core/source_session_coordinator.hpp"
#include "core/source_session_verifier.hpp"
#include "platform/persistence/sqlite_source_session_store.hpp"
#include "../regression/source_session_test_support.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <sqlite3.h>
#include <unistd.h>

using dockstation::core::Event;
using dockstation::core::RegistrySourceSessionVerifier;
using dockstation::core::SourceCursorReadStatus;
using dockstation::core::SourceRegistrationState;
using dockstation::core::SourceSessionConfirmationStatus;
using dockstation::core::SourceSessionCoordinator;
using dockstation::core::SourceSessionVerificationStatus;
using dockstation::platform::persistence::SqliteDatabase;
using dockstation::platform::persistence::SqliteDatabaseConfig;
using dockstation::platform::persistence::SqliteSourceRegistrationWriteStatus;
using dockstation::platform::persistence::SqliteSourceSessionStore;
using dockstation::verification::SourceSessionEvidenceTestAccess;

namespace {

int checks = 0;
int failures = 0;

constexpr std::string_view kStation{"station-1"};
constexpr std::string_view kSource{"esp32.main"};
constexpr std::string_view kProof{"0123456789abcdef0123456789abcdef"};

void expect(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << description << '\n';
    } else {
        ++failures;
        std::cerr << "FAIL - " << description << '\n';
    }
}

bool execute(sqlite3* database, const char* sql) {
    char* error_message = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &error_message);
    if (status != SQLITE_OK) {
        std::cerr << "SQLite status " << status;
        if (error_message != nullptr) {
            std::cerr << ": " << error_message;
        }
        std::cerr << '\n';
    }
    sqlite3_free(error_message);
    return status == SQLITE_OK;
}

bool query_integer(sqlite3* database, const char* sql, sqlite3_int64& value) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    bool valid = false;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int64(statement, 0);
        valid = sqlite3_step(statement) == SQLITE_DONE;
    }
    const int finalize_status = sqlite3_finalize(statement);
    return valid && finalize_status == SQLITE_OK;
}

bool create_version_one_database(const std::string& path) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        if (database != nullptr) (void)sqlite3_close(database);
        return false;
    }
    const bool created = execute(database,
        "CREATE TABLE schema_migrations ("
        " version INTEGER PRIMARY KEY NOT NULL CHECK(version > 0),"
        " applied_at_utc TEXT NOT NULL CHECK(length(applied_at_utc) > 0)"
        ");"
        "INSERT INTO schema_migrations(version, applied_at_utc) "
        "VALUES (1, '2026-11-15T00:00:00.000Z');"
        "PRAGMA application_id=0x4453544E;"
        "PRAGMA user_version=1;");
    const int close_status = sqlite3_close(database);
    return created && close_status == SQLITE_OK;
}

void remove_database_files(const std::string& path) {
    (void)std::remove((path + "-journal").c_str());
    (void)std::remove((path + "-wal").c_str());
    (void)std::remove((path + "-shm").c_str());
    (void)std::remove(path.c_str());
}

dockstation::core::SourceSessionEvidence make_evidence(
    std::string_view source_instance,
    std::string_view peer_identity) {
    return SourceSessionEvidenceTestAccess::make(
        kSource, source_instance, peer_identity, kProof);
}

Event make_event(std::string_view source_instance) {
    Event event{};
    event.source = kSource;
    event.source_instance = source_instance;
    return event;
}

bool cursor_is(
    const dockstation::core::SourceCursorReadResult& cursor,
    std::string_view instance,
    std::optional<std::uint64_t> sequence) {
    return cursor.status == SourceCursorReadStatus::Found &&
           cursor.cursor.source == kSource &&
           cursor.cursor.source_instance.has_value() &&
           *cursor.cursor.source_instance == instance &&
           cursor.cursor.last_sequence == sequence;
}

}  // namespace

int main() {
    char directory_template[] = "/tmp/dockstation_source_session_XXXXXX";
    char* created_directory = ::mkdtemp(directory_template);
    if (created_directory == nullptr) {
        std::cerr << "Could not create source-session test directory\n";
        return EXIT_FAILURE;
    }
    const std::string directory{created_directory};
    const std::string database_path = directory + "/station.sqlite3";
    const std::string legacy_path = directory + "/legacy-v1.sqlite3";

    SqliteDatabase database{};
    const auto opened = database.open(SqliteDatabaseConfig{database_path, 100});
    expect(static_cast<bool>(opened) && opened.schema_version == 2,
           "fresh database applies schema migrations 1 and 2");
    if (!opened) {
        remove_database_files(database_path);
        (void)::rmdir(directory.c_str());
        return EXIT_FAILURE;
    }

    sqlite3_int64 schema_version = 0;
    sqlite3_int64 migration_count = 0;
    expect(query_integer(database.native_handle(), "PRAGMA user_version;", schema_version) &&
               schema_version == 2 &&
               query_integer(database.native_handle(),
                   "SELECT count(*) FROM schema_migrations WHERE version IN (1,2);",
                   migration_count) &&
               migration_count == 2,
           "schema version and migration ledger agree");

    SqliteSourceSessionStore store{database};
    const std::string peer_identity{"peer\0identity", 13U};
    const auto created = store.create_source_registration(
        kStation, kSource, "revision-1", peer_identity);
    expect(created.status == SqliteSourceRegistrationWriteStatus::Created,
           "creates a source registration through the local provisioning boundary");

    const auto duplicate_identity = store.create_source_registration(
        kStation, "esp32.other", "revision-1", peer_identity);
    expect(duplicate_identity.status == SqliteSourceRegistrationWriteStatus::Conflict,
           "rejects reuse of an opaque peer identity");

    const auto duplicate_source = store.create_source_registration(
        kStation, kSource, "revision-2", "another-peer-id");
    expect(duplicate_source.status == SqliteSourceRegistrationWriteStatus::Conflict,
           "rejects a second registration for the same logical source");

    const auto invalid_registration = store.create_source_registration(
        kStation, kSource, " ", "bad-peer");
    expect(invalid_registration.status ==
               SqliteSourceRegistrationWriteStatus::InvalidArgument,
           "rejects blank registration revision before SQL");

    const auto unknown_lookup = store.find_by_peer_identity("not-registered");
    expect(unknown_lookup.status ==
               dockstation::core::SourceRegistrationLookupStatus::NotFound,
           "unknown peer identity is distinct from repository error");

    const auto lookup = store.find_by_peer_identity(peer_identity);
    expect(lookup.status == dockstation::core::SourceRegistrationLookupStatus::Found &&
               lookup.record.station_id == kStation &&
               lookup.record.source == kSource &&
               lookup.record.registration_revision == "revision-1" &&
               lookup.record.state == SourceRegistrationState::Active,
           "looks up station, source, revision and ACTIVE state by exact binary identity");

    RegistrySourceSessionVerifier verifier{store, kStation};
    RegistrySourceSessionVerifier wrong_station_verifier{store, "station-2"};
    const auto wrong_station = wrong_station_verifier.verify_source_session(
        make_evidence("boot-1", peer_identity));
    expect(wrong_station.status == SourceSessionVerificationStatus::Rejected,
           "registry policy does not authorize a peer for another local Station ID");

    const auto no_cursor = store.read_source_cursor(kSource);
    expect(no_cursor.status == SourceCursorReadStatus::NotFound,
           "source cursor lookup distinguishes a new source from an error");

    SourceSessionCoordinator coordinator{verifier, store, store};
    auto evidence_one = make_evidence("boot-1", peer_identity);
    const auto first = coordinator.confirm(make_event("boot-1"), evidence_one);
    expect(first.status ==
               dockstation::core::SourceSessionCoordinatorStatus::Confirmed &&
               first.cursor.source == kSource &&
               first.cursor.source_instance ==
                   std::optional<std::string_view>{"boot-1"} &&
               !first.cursor.last_sequence,
           "first verified source session is durably confirmed with no sequence");

    const auto same = coordinator.confirm(make_event("boot-1"), evidence_one);
    expect(same.status ==
               dockstation::core::SourceSessionCoordinatorStatus::AlreadyCurrent &&
               same.cursor.source_instance ==
                   std::optional<std::string_view>{"boot-1"} &&
               !same.cursor.last_sequence,
           "repeated confirmation of the same session does not reset its cursor");

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const std::string maximum_text = std::to_string(maximum);
    sqlite3_stmt* maximum_update = nullptr;
    const bool maximum_prepared =
        sqlite3_prepare_v2(database.native_handle(),
            "UPDATE source_cursors SET last_sequence=?1 WHERE source='esp32.main';",
            -1, &maximum_update, nullptr) == SQLITE_OK;
    const bool set_max_sequence = maximum_prepared &&
        sqlite3_bind_text(maximum_update, 1, maximum_text.data(),
                          static_cast<int>(maximum_text.size()),
                          SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(maximum_update) == SQLITE_DONE;
    const int maximum_finalize =
        maximum_update == nullptr ? SQLITE_OK : sqlite3_finalize(maximum_update);
    const auto max_sequence = store.read_source_cursor(kSource);
    expect(set_max_sequence && maximum_finalize == SQLITE_OK &&
               cursor_is(max_sequence, "boot-1", maximum),
           "reads the full uint64 sequence range without signed INTEGER truncation");

    const auto same_with_sequence =
        coordinator.confirm(make_event("boot-1"), evidence_one);
    expect(same_with_sequence.status ==
               dockstation::core::SourceSessionCoordinatorStatus::AlreadyCurrent &&
               same_with_sequence.cursor.last_sequence == maximum,
           "same-session confirmation preserves the previously accepted sequence");

    auto evidence_two = make_evidence("boot-2", peer_identity);
    const auto second = coordinator.confirm(make_event("boot-2"), evidence_two);
    expect(second.status ==
               dockstation::core::SourceSessionCoordinatorStatus::Confirmed &&
               second.cursor.source_instance ==
                   std::optional<std::string_view>{"boot-2"} &&
               !second.cursor.last_sequence,
           "a verified new source instance atomically replaces the cursor and clears sequence");

    const auto stale_expected = store.read_source_cursor(kSource);
    const bool changed_cursor = execute(database.native_handle(),
        "UPDATE source_cursors SET last_sequence='42' WHERE source='esp32.main';");
    const auto stale_confirmation = store.confirm_source_session(
        stale_expected, {"esp32.main", "boot-3", "revision-1"});
    const auto cursor_after_race = store.read_source_cursor(kSource);
    expect(changed_cursor &&
               stale_confirmation.status == SourceSessionConfirmationStatus::CursorChanged &&
               cursor_is(cursor_after_race, "boot-2", std::optional<std::uint64_t>{42U}),
           "cursor compare-and-set rejects an intervening update without overwriting it");

    auto evidence_three = make_evidence("boot-3", peer_identity);
    const auto verified_before_disable =
        verifier.verify_source_session(evidence_three);
    const auto expected_before_disable = store.read_source_cursor(kSource);
    const auto disabled = store.change_source_state(
        kSource, "revision-1", "revision-2", SourceRegistrationState::Disabled);
    const auto commit_after_disable = store.confirm_source_session(
        expected_before_disable, verified_before_disable.session);
    expect(disabled.status == SqliteSourceRegistrationWriteStatus::Updated &&
               commit_after_disable.status ==
                   SourceSessionConfirmationStatus::RegistrationChanged,
           "transaction re-checks ACTIVE registration revision before changing a cursor");

    const auto disabled_verification = verifier.verify_source_session(evidence_three);
    expect(disabled_verification.status == SourceSessionVerificationStatus::Rejected,
           "disabled source cannot establish a new session");

    const auto reactivated = store.change_source_state(
        kSource, "revision-2", "revision-3", SourceRegistrationState::Active);
    const auto after_reactivation =
        coordinator.confirm(make_event("boot-3"), evidence_three);
    expect(reactivated.status == SqliteSourceRegistrationWriteStatus::Updated &&
               after_reactivation.status ==
                   dockstation::core::SourceSessionCoordinatorStatus::Confirmed &&
               after_reactivation.cursor.source_instance ==
                   std::optional<std::string_view>{"boot-3"} &&
               !after_reactivation.cursor.last_sequence,
           "explicit reactivation with a new revision permits a fresh verified session");

    const auto revoked = store.change_source_state(
        kSource, "revision-3", "revision-4", SourceRegistrationState::Revoked);
    const auto revive_revoked = store.change_source_state(
        kSource, "revision-4", "revision-5", SourceRegistrationState::Active);
    expect(revoked.status == SqliteSourceRegistrationWriteStatus::Updated &&
               revive_revoked.status ==
                   SqliteSourceRegistrationWriteStatus::RevisionMismatch,
           "revoked registration is terminal and cannot be reactivated");

    const auto revoked_verification = verifier.verify_source_session(evidence_three);
    expect(revoked_verification.status == SourceSessionVerificationStatus::Rejected,
           "revoked peer is denied by the source-session verifier");

    expect(database.close(), "closes the persistent database cleanly");
    const auto reopened = database.open(SqliteDatabaseConfig{database_path, 100});
    const auto persisted_cursor = store.read_source_cursor(kSource);
    const auto persisted_lookup = store.find_by_peer_identity(peer_identity);
    expect(static_cast<bool>(reopened) &&
               persisted_cursor.status == SourceCursorReadStatus::Found &&
               persisted_cursor.cursor.source_instance ==
                   std::optional<std::string_view>{"boot-3"} &&
               !persisted_cursor.cursor.last_sequence &&
               persisted_lookup.status ==
                   dockstation::core::SourceRegistrationLookupStatus::Found &&
               persisted_lookup.record.state == SourceRegistrationState::Revoked &&
               persisted_lookup.record.registration_revision == "revision-4",
           "source registration state and cursor persist across close/reopen");

    expect(create_version_one_database(legacy_path),
           "creates a version-one database fixture");
    SqliteDatabase upgraded{};
    const auto upgrade_result = upgraded.open(SqliteDatabaseConfig{legacy_path, 100});
    sqlite3_int64 upgraded_version = 0;
    sqlite3_int64 cursor_table_count = 0;
    expect(static_cast<bool>(upgrade_result) && upgrade_result.schema_version == 2 &&
               query_integer(upgraded.native_handle(), "PRAGMA user_version;", upgraded_version) &&
               upgraded_version == 2 &&
               query_integer(upgraded.native_handle(),
                   "SELECT count(*) FROM sqlite_schema WHERE type='table' "
                   "AND name='source_cursors';", cursor_table_count) &&
               cursor_table_count == 1,
           "version-one database upgrades transactionally to source-session schema v2");

    const bool upgraded_close = upgraded.close();
    const bool final_close = database.close();
    expect(upgraded_close && final_close, "closes both database connections");

    remove_database_files(database_path);
    remove_database_files(legacy_path);
    (void)::rmdir(directory.c_str());

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
