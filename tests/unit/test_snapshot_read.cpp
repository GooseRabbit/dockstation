#include <cstdlib>
#include <iostream>
#include <string_view>

#include "core/state_transitions.hpp"

using namespace dockstation::core;

namespace {
int failures = 0;

void check(bool condition, std::string_view name) {
    std::cout << (condition ? "PASS" : "FAIL") << " - " << name << '\n';
    if (!condition) ++failures;
}

class FakeSnapshotReader final : public StationStateSnapshotReader {
public:
    SnapshotReadPortResult result{};
    int calls{0};

    SnapshotReadPortResult read_snapshot() noexcept override {
        ++calls;
        return result;
    }
};
}  // namespace

int main() {
    FakeSnapshotReader reader;
    reader.result.status = SnapshotReadPortStatus::Found;
    reader.result.snapshot = StationStateSnapshot{StationState::PoweredOff, 0, {}};

    auto result = get_snapshot(reader);
    check(result.status == SnapshotReadStatus::Available && result.snapshot.has_value() &&
          result.snapshot->state == StationState::PoweredOff &&
          result.snapshot->revision == 0, "initial snapshot");
    reader.result.snapshot = StationStateSnapshot{StationState::Waiting, 4, "transition-4"};
    result = get_snapshot(reader);
    check(result.status == SnapshotReadStatus::Available && result.snapshot.has_value() &&
          result.snapshot->revision == 4 &&
          result.snapshot->transition_id == "transition-4",
          "snapshot after transitions");

    reader.result.status = SnapshotReadPortStatus::NotFound;
    reader.result.snapshot.reset();
    result = get_snapshot(reader);
    check(result.status == SnapshotReadStatus::NotFound && !result.snapshot.has_value(),
          "missing snapshot has no value");

    reader.result.status = SnapshotReadPortStatus::Error;
    result = get_snapshot(reader);
    check(result.status == SnapshotReadStatus::ReadError && !result.snapshot.has_value(),
          "repository error has no value");

    reader.result.status = SnapshotReadPortStatus::Found;
    reader.result.snapshot = StationStateSnapshot{static_cast<StationState>(255), 1, "transition-1"};
    result = get_snapshot(reader);
    check(result.status == SnapshotReadStatus::InvalidSnapshot && !result.snapshot.has_value(),
          "unknown state rejected");

    reader.result.snapshot = StationStateSnapshot{StationState::Waiting, 0, "transition-0"};
    result = get_snapshot(reader);
    check(result.status == SnapshotReadStatus::InvalidSnapshot && !result.snapshot.has_value(),
          "revision zero with transition id rejected");

    reader.result.snapshot = StationStateSnapshot{StationState::Waiting, 2, {}};
    result = get_snapshot(reader);
    check(result.status == SnapshotReadStatus::InvalidSnapshot && !result.snapshot.has_value(),
          "positive revision without transition id rejected");

    reader.result.snapshot = StationStateSnapshot{StationState::Waiting, 2, "  \t"};
    result = get_snapshot(reader);
    check(result.status == SnapshotReadStatus::InvalidSnapshot && !result.snapshot.has_value(),
          "whitespace transition ID rejected");

    reader.result.status = static_cast<SnapshotReadPortStatus>(255);
    result = get_snapshot(reader);
    check(result.status == SnapshotReadStatus::ReadError && !result.snapshot.has_value(),
          "unknown repository status fails closed");

    reader.result.status = SnapshotReadPortStatus::Found;
    reader.result.snapshot.reset();
    result = get_snapshot(reader);
    check(result.status == SnapshotReadStatus::InvalidSnapshot && !result.snapshot.has_value(),
          "found status without snapshot is invalid");

    check(reader.calls == 10, "exactly one repository read per call");

    std::cout << "Assertions: 11; failures: " << failures << std::endl;
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
