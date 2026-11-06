#include "core/recovery.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

using namespace dockstation::core;

namespace {

int checks = 0;

void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class FakeRecoveryRepository final : public RecoveryRepository {
public:
    RecoverySnapshotLoadResult load_snapshot() noexcept override {
        ++load_calls;
        call_order.push_back("load");
        return loaded;
    }

    RecoveryMarkStatus mark_recovery_in_progress(
        const StationStateSnapshot& expected) noexcept override {
        ++mark_calls;
        call_order.push_back("mark");
        marked_snapshot = expected;
        return mark_status;
    }

    RecoverySnapshotLoadResult loaded{};
    RecoveryMarkStatus mark_status{RecoveryMarkStatus::Marked};
    int load_calls{0};
    int mark_calls{0};
    StationStateSnapshot marked_snapshot{};
    std::vector<const char*> call_order{};
};

class FakeReportRequester final : public RecoveryReportRequester {
public:
    RecoveryReportRequestStatus request_fresh_reports(
        const StationStateSnapshot& snapshot) noexcept override {
        ++calls;
        call_order->push_back("reports");
        requested_snapshot = snapshot;
        return status;
    }

    RecoveryReportRequestStatus status{RecoveryReportRequestStatus::Accepted};
    int calls{0};
    StationStateSnapshot requested_snapshot{};
    std::vector<const char*>* call_order{nullptr};
};

void reset(FakeRecoveryRepository& repository, FakeReportRequester& requester) {
    repository.loaded = {};
    repository.loaded.status = RecoverySnapshotLoadStatus::Found;
    repository.loaded.snapshot = {StationState::EnclosedService, 42, "transition-42"};
    repository.mark_status = RecoveryMarkStatus::Marked;
    repository.load_calls = 0;
    repository.mark_calls = 0;
    repository.marked_snapshot = {};
    repository.call_order.clear();
    requester.status = RecoveryReportRequestStatus::Accepted;
    requester.calls = 0;
    requester.requested_snapshot = {};
    requester.call_order = &repository.call_order;
}

}  // namespace

int main() {
    FakeRecoveryRepository repository;
    FakeReportRequester requester;
    requester.call_order = &repository.call_order;
    RecoveryStartResult result{};

    // Successful start: load -> durable marker -> report requests.
    reset(repository, requester);
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::Started, "new recovery started");
    expect(result.has_snapshot, "loaded snapshot returned");
    expect(result.previous_snapshot.state == StationState::EnclosedService &&
           result.previous_snapshot.revision == 42 &&
           result.previous_snapshot.transition_id == "transition-42",
           "previous snapshot is preserved");
    expect(repository.load_calls == 1 && repository.mark_calls == 1 && requester.calls == 1,
           "each port called once on success");
    expect(repository.call_order.size() == 3 &&
           repository.call_order[0] == std::string_view{"load"} &&
           repository.call_order[1] == std::string_view{"mark"} &&
           repository.call_order[2] == std::string_view{"reports"},
           "persistent marker precedes subsystem report requests");
    expect(repository.marked_snapshot.revision == 42 &&
           requester.requested_snapshot.transition_id == "transition-42",
           "same expected snapshot passed to both ports");

    // Repeated entry is idempotent and does not create a second recovery marker.
    reset(repository, requester);
    repository.mark_status = RecoveryMarkStatus::AlreadyInProgress;
    requester.status = RecoveryReportRequestStatus::AlreadyPending;
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::AlreadyInProgress,
           "existing recovery and pending report request remain in progress");
    expect(repository.mark_calls == 1 && requester.calls == 1,
           "repeat checks existing marker and report request once");

    reset(repository, requester);
    repository.mark_status = RecoveryMarkStatus::AlreadyInProgress;
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::AlreadyInProgress,
           "already active recovery can re-request fresh reports");

    // Missing, failed or unknown snapshot lookup stops before marker/report side effects.
    reset(repository, requester);
    repository.loaded.status = RecoverySnapshotLoadStatus::NotFound;
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::SnapshotNotFound && !result.has_snapshot,
           "missing persisted snapshot is explicit");
    expect(repository.mark_calls == 0 && requester.calls == 0,
           "no downstream side effects without snapshot");

    reset(repository, requester);
    repository.loaded.status = RecoverySnapshotLoadStatus::Error;
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::RepositoryError && !result.has_snapshot,
           "snapshot read error fails closed");
    expect(repository.mark_calls == 0 && requester.calls == 0,
           "snapshot read error prevents recovery requests");

    reset(repository, requester);
    repository.loaded.status = static_cast<RecoverySnapshotLoadStatus>(255);
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::RepositoryError,
           "unknown snapshot lookup status fails closed");

    // Corrupt state/revision linkage is rejected without writing a recovery marker.
    reset(repository, requester);
    repository.loaded.snapshot.state = static_cast<StationState>(255);
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::InvalidSnapshot,
           "unknown persisted station state rejected");
    expect(repository.mark_calls == 0 && requester.calls == 0,
           "invalid snapshot cannot start recovery");

    reset(repository, requester);
    repository.loaded.snapshot.revision = 1;
    repository.loaded.snapshot.transition_id = {};
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::InvalidSnapshot,
           "positive revision requires transition id");

    reset(repository, requester);
    repository.loaded.snapshot.revision = 1;
    repository.loaded.snapshot.transition_id = " \t";
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::InvalidSnapshot,
           "positive revision rejects whitespace-only transition id");

    reset(repository, requester);
    repository.loaded.snapshot.revision = 0;
    repository.loaded.snapshot.transition_id = "unexpected-transition";
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::InvalidSnapshot,
           "initial revision cannot carry a transition id");

    // Repository must confirm the marker before any report request is issued.
    reset(repository, requester);
    repository.mark_status = RecoveryMarkStatus::SnapshotChanged;
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::SnapshotChanged,
           "snapshot race is reported");
    expect(requester.calls == 0, "snapshot race prevents report request");

    reset(repository, requester);
    repository.mark_status = RecoveryMarkStatus::Error;
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::RepositoryError,
           "recovery marker persistence error is reported");
    expect(requester.calls == 0, "failed recovery marker prevents report request");

    reset(repository, requester);
    repository.mark_status = static_cast<RecoveryMarkStatus>(255);
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::RepositoryError,
           "unknown marker status fails closed");
    expect(requester.calls == 0, "unknown marker status prevents report request");

    // Report-request outcomes remain distinct; the persistent marker stays visible for retry.
    reset(repository, requester);
    requester.status = RecoveryReportRequestStatus::Rejected;
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::ReportRequestRejected && result.has_snapshot,
           "report request rejection is explicit");

    reset(repository, requester);
    requester.status = RecoveryReportRequestStatus::Unknown;
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::ReportRequestUnknown,
           "unknown report request result is not accepted");

    reset(repository, requester);
    requester.status = RecoveryReportRequestStatus::Error;
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::ReportRequestError,
           "report request error is explicit");

    reset(repository, requester);
    requester.status = static_cast<RecoveryReportRequestStatus>(255);
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::ReportRequestError,
           "unknown report request status fails closed");

    // The initial PoweredOff snapshot is valid without a previous transition.
    reset(repository, requester);
    repository.loaded.snapshot = {StationState::PoweredOff, 0, {}};
    result = begin_recovery(repository, requester);
    expect(result.status == RecoveryStartStatus::Started,
           "initial zero-revision snapshot is valid");

    std::cout << "PASS: " << checks << " assertions\n";
    return 0;
}
