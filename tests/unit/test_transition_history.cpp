#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/state_transitions.hpp"

using namespace dockstation::core;

namespace {
int failures = 0;

void check(bool condition, std::string_view name) {
    std::cout << (condition ? "PASS" : "FAIL") << " - " << name << '\\n';
    if (!condition) ++failures;
}

TransitionHistoryRecord record(std::uint64_t revision,
                               StationState from,
                               StationState to,
                               std::string event_id,
                               std::string transition_id) {
    return {from, to, revision, std::move(event_id), std::move(transition_id), {}, {}};
}

class FakeHistoryReader final : public TransitionHistoryReader {
public:
    TransitionHistoryPortResult result{};
    TransitionHistoryFilter last_filter{};
    std::size_t calls{0};

    TransitionHistoryPortResult read_transition_history(
        const TransitionHistoryFilter& filter) noexcept override {
        ++calls;
        last_filter = filter;
        return std::move(result);
    }
};
}  // namespace

int main() {
    FakeHistoryReader reader;
    reader.result.status = TransitionHistoryPortStatus::Complete;
    auto result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::Empty && result.records.empty(),
          "empty complete history");

    reader.result.records = {
        record(1, StationState::PoweredOff, StationState::Startup, "event-1", "transition-1"),
        record(2, StationState::Startup, StationState::SelfTest, "event-2", "transition-2"),
        record(3, StationState::SelfTest, StationState::Waiting, "event-3", "transition-3")
    };
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::Available && result.records.size() == 3 &&
          result.records[0].revision == 1 && result.records[2].revision == 3,
          "ordered complete history");

    TransitionHistoryFilter filter{};
    filter.minimum_revision = 2;
    filter.maximum_revision = 3;
    filter.resulting_state = StationState::Waiting;
    reader.result.records = {
        record(3, StationState::SelfTest, StationState::Waiting, "event-3", "transition-3")
    };
    result = get_transition_history(filter, reader);
    check(result.status == TransitionHistoryStatus::Available && result.records.size() == 1 &&
          reader.last_filter.minimum_revision == 2 &&
          reader.last_filter.maximum_revision == 3 &&
          reader.last_filter.resulting_state == StationState::Waiting,
          "inclusive revision and destination filters");

    const std::size_t before_invalid_filter = reader.calls;
    filter.minimum_revision = 9;
    filter.maximum_revision = 4;
    result = get_transition_history(filter, reader);
    check(result.status == TransitionHistoryStatus::InvalidFilter &&
          reader.calls == before_invalid_filter,
          "invalid revision range does not call reader");

    filter.minimum_revision.reset();
    filter.maximum_revision.reset();
    filter.resulting_state = static_cast<StationState>(255);
    result = get_transition_history(filter, reader);
    check(result.status == TransitionHistoryStatus::InvalidFilter,
          "unknown state in filter rejected");
    filter.resulting_state.reset();

    reader.result.status = TransitionHistoryPortStatus::Incomplete;
    reader.result.records = {
        record(3, StationState::SelfTest, StationState::Waiting, "event-3", "transition-3")
    };
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::Incomplete && result.records.empty(),
          "partial history is not exposed");

    reader.result.status = TransitionHistoryPortStatus::Error;
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::ReadError && result.records.empty(),
          "reader error is not exposed as history");

    reader.result.status = static_cast<TransitionHistoryPortStatus>(255);
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::ReadError && result.records.empty(),
          "unknown port status fails closed");

    reader.result.status = TransitionHistoryPortStatus::Complete;
    reader.result.records = {
        record(0, StationState::PoweredOff, StationState::Startup, "event-0", "transition-0")
    };
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::InvalidHistory,
          "zero revision rejected");

    reader.result.records = {
        record(1, static_cast<StationState>(255), StationState::Startup, "event-1", "transition-1")
    };
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::InvalidHistory,
          "unknown state in record rejected");

    reader.result.records = {
        record(1, StationState::Startup, StationState::Startup, "event-1", "transition-1")
    };
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::InvalidHistory,
          "transition with identical states rejected");

    reader.result.records = {
        record(1, StationState::PoweredOff, StationState::Startup, "  ", "transition-1")
    };
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::InvalidHistory,
          "empty event ID rejected");

    reader.result.records = {
        record(1, StationState::PoweredOff, StationState::Startup, "event-1", " ")
    };
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::InvalidHistory,
          "empty transition ID rejected");

    auto malformed_reason = record(1, StationState::PoweredOff, StationState::Startup,
                                   "event-1", "transition-1");
    malformed_reason.reason_code = "FAIL-001";
    reader.result.records = {malformed_reason};
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::InvalidHistory,
          "unpaired reason rejected");

    malformed_reason.reason_text = " ";
    reader.result.records = {malformed_reason};
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::InvalidHistory,
          "blank reason text rejected");

    malformed_reason.reason_text = "Known reason";
    reader.result.records = {malformed_reason};
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::Available &&
          result.records[0].reason_code == "FAIL-001" &&
          result.records[0].reason_text == "Known reason",
          "valid reason pair preserved");

    reader.result.records = {
        record(2, StationState::Startup, StationState::SelfTest, "event-2", "transition-2"),
        record(1, StationState::PoweredOff, StationState::Startup, "event-1", "transition-1")
    };
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::InvalidHistory,
          "unordered revisions rejected");

    reader.result.records = {
        record(1, StationState::PoweredOff, StationState::Startup, "event-1", "transition-1"),
        record(1, StationState::Startup, StationState::SelfTest, "event-2", "transition-2")
    };
    result = get_transition_history({}, reader);
    check(result.status == TransitionHistoryStatus::InvalidHistory,
          "duplicate revision rejected");

    reader.result.records = {
        record(4, StationState::SelfTest, StationState::Waiting, "event-4", "transition-4")
    };
    filter.minimum_revision = 1;
    filter.maximum_revision = 3;
    result = get_transition_history(filter, reader);
    check(result.status == TransitionHistoryStatus::InvalidHistory,
          "reader record outside filter rejected");

    std::cout << "Assertions: 19; failures: " << failures << std::endl;
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
