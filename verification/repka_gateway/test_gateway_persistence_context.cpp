#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "application/linux/gateway_persistence_context.hpp"
#include "core/source_session_verifier.hpp"
#include "core/subsystem_status_event_processor.hpp"

namespace {

int failures = 0;
int checks = 0;

void expect(bool condition, const char* name) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << name << '\n';
    } else {
        std::cerr << "FAIL - " << name << '\n';
        ++failures;
    }
}

void update_maximum(std::atomic<int>& maximum, int candidate) {
    int observed = maximum.load();
    while (candidate > observed &&
           !maximum.compare_exchange_weak(observed, candidate)) {
    }
}

}  // namespace

int main() {
    using dockstation::application::linux_app::GatewayPersistenceContext;
    using dockstation::core::SourceRegistrationLookupStatus;
    using dockstation::core::SubsystemRegistrationLookupStatus;

    const std::string database_path =
        "/tmp/dockstation_gateway_persistence_context_" +
        std::to_string(static_cast<long long>(::getpid())) + ".sqlite3";
    const std::string missing_parent_path =
        "/tmp/dockstation_gateway_persistence_context_missing_" +
        std::to_string(static_cast<long long>(::getpid())) +
        "/state.sqlite3";
    (void)std::remove(database_path.c_str());

    GatewayPersistenceContext context;
    const auto relative_path_result = context.open("relative.sqlite3");
    expect(relative_path_result.status ==
               dockstation::platform::persistence::SqliteDatabaseStatus::InvalidPath &&
               !context.is_open(),
           "rejects a relative database path without opening the connection");

    const auto missing_parent_result = context.open(missing_parent_path);
    expect(missing_parent_result.status ==
               dockstation::platform::persistence::SqliteDatabaseStatus::OpenFailed &&
               !context.is_open(),
           "fails closed when the persistent state directory is absent");

    const auto open_result = context.open(database_path);
    expect(static_cast<bool>(open_result) && context.is_open(),
           "opens one local SQLite connection");
    expect(context.schema_version() ==
               dockstation::platform::persistence::SqliteDatabase::kCurrentSchemaVersion,
           "applies the current schema before exposing persistence adapters");

    bool source_registration_absent = false;
    bool subsystem_registration_absent = false;
    const bool stores_invoked = context.with_serialized_stores(
        [&source_registration_absent, &subsystem_registration_absent](
            dockstation::platform::persistence::SqliteSourceSessionStore& source_store,
            dockstation::platform::persistence::SqliteSubsystemStatusStore& status_store) {
            source_registration_absent =
                source_store.find_by_peer_identity("peer-test").status ==
                SourceRegistrationLookupStatus::NotFound;
            subsystem_registration_absent =
                status_store.find_subsystem("station-test", "subsystem-test").status ==
                SubsystemRegistrationLookupStatus::NotFound;
        });
    expect(stores_invoked && source_registration_absent &&
               subsystem_registration_absent,
           "exposes both adapters through one guarded callback without provisioning data");

    constexpr int kWorkerCount = 6;
    constexpr int kCallbacksPerWorker = 40;
    std::atomic<int> active_callbacks{0};
    std::atomic<int> maximum_active_callbacks{0};
    std::atomic<int> callback_count{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkerCount);
    for (int worker = 0; worker < kWorkerCount; ++worker) {
        workers.emplace_back([&context, &active_callbacks,
                              &maximum_active_callbacks, &callback_count]() {
            for (int iteration = 0; iteration < kCallbacksPerWorker; ++iteration) {
                const bool invoked = context.with_serialized_stores(
                    [&active_callbacks, &maximum_active_callbacks, &callback_count](
                        dockstation::platform::persistence::SqliteSourceSessionStore&,
                        dockstation::platform::persistence::SqliteSubsystemStatusStore&) {
                        const int active = active_callbacks.fetch_add(1) + 1;
                        update_maximum(maximum_active_callbacks, active);
                        std::this_thread::yield();
                        active_callbacks.fetch_sub(1);
                        callback_count.fetch_add(1);
                    });
                if (!invoked) {
                    callback_count.fetch_add(-1000000);
                    return;
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    expect(callback_count.load() == kWorkerCount * kCallbacksPerWorker &&
               maximum_active_callbacks.load() == 1,
           "serializes concurrent access to both adapters through one mutex");

    expect(context.close() && !context.is_open(),
           "closes the database after adapter work has completed");
    int callback_after_close = 0;
    const bool closed_access = context.with_serialized_stores(
        [&callback_after_close](
            dockstation::platform::persistence::SqliteSourceSessionStore&,
            dockstation::platform::persistence::SqliteSubsystemStatusStore&) {
            ++callback_after_close;
        });
    expect(!closed_access && callback_after_close == 0,
           "does not expose adapters while the database is closed");
    expect(context.close(),
           "supports an idempotent close during orderly shutdown");

    const auto reopen_result = context.open(database_path);
    expect(static_cast<bool>(reopen_result) &&
               context.schema_version() ==
                   dockstation::platform::persistence::SqliteDatabase::kCurrentSchemaVersion,
           "reopens the same database with its migrated schema intact");
    expect(context.close(),
           "closes the reopened database cleanly");

    (void)std::remove(database_path.c_str());
    const std::string journal_path = database_path + "-journal";
    (void)std::remove(journal_path.c_str());

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
