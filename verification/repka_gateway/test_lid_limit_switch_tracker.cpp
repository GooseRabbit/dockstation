#include "lid_limit_switch_tracker.hpp"

#include <cstdlib>
#include <iostream>

using namespace dockstation::firmware::esp32;

namespace {
int checks = 0;
int failures = 0;

void expect(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << description << '\n';
    } else {
        std::cerr << "FAIL - " << description << '\n';
        ++failures;
    }
}

LidLimitSwitchConfig configured_pair(std::uint64_t debounce_ms = 10U) {
    return {
        {true, LimitSwitchActiveLevel::High, debounce_ms},
        {true, LimitSwitchActiveLevel::Low, debounce_ms}
    };
}

}  // namespace

int main() {
    {
        LidLimitSwitchTracker tracker;
        const auto snapshot = tracker.update(true, false, 100U);
        expect(snapshot.open_limit == LimitSwitchSignalStatus::NotConfigured &&
                   snapshot.closed_limit == LimitSwitchSignalStatus::NotConfigured &&
                   snapshot.position == LidPositionStatus::NotConfigured,
               "default tracker reports NOT_CONFIGURED without assuming wiring");
    }

    {
        LidLimitSwitchTracker tracker(configured_pair());
        auto snapshot = tracker.update(true, true, 100U);
        expect(snapshot.position == LidPositionStatus::Transitioning,
               "initial raw samples are not trusted before debounce");

        snapshot = tracker.update(true, true, 109U);
        expect(snapshot.position == LidPositionStatus::Transitioning,
               "input remains TRANSITIONING until the full debounce interval");

        snapshot = tracker.update(true, true, 110U);
        expect(snapshot.open_limit == LimitSwitchSignalStatus::Active &&
                   snapshot.closed_limit == LimitSwitchSignalStatus::Inactive &&
                   snapshot.position == LidPositionStatus::Open,
               "explicit high-active/low-active profiles confirm OPEN");
    }

    {
        LidLimitSwitchTracker tracker(configured_pair());
        tracker.update(true, true, 0U);
        auto snapshot = tracker.update(true, true, 10U);
        expect(snapshot.position == LidPositionStatus::Open,
               "test setup reaches stable OPEN");

        snapshot = tracker.update(false, true, 11U);
        expect(snapshot.position == LidPositionStatus::Transitioning,
               "changed raw inputs start a new debounce interval");

        snapshot = tracker.update(false, true, 21U);
        expect(snapshot.open_limit == LimitSwitchSignalStatus::Inactive &&
                   snapshot.closed_limit == LimitSwitchSignalStatus::Inactive &&
                   snapshot.position == LidPositionStatus::Transitioning,
               "two stable inactive end-stops do not claim OPEN or CLOSED");
    }

    {
        LidLimitSwitchTracker tracker(configured_pair());
        tracker.update(false, false, 0U);
        auto snapshot = tracker.update(false, false, 10U);
        expect(snapshot.position == LidPositionStatus::Closed,
               "explicit active-low closed switch confirms CLOSED");

        snapshot = tracker.update(false, true, 11U);
        expect(snapshot.position == LidPositionStatus::Transitioning,
               "leaving CLOSED immediately removes the confirmed endpoint state");

        snapshot = tracker.update(false, true, 21U);
        expect(snapshot.open_limit == LimitSwitchSignalStatus::Inactive &&
                   snapshot.closed_limit == LimitSwitchSignalStatus::Inactive &&
                   snapshot.position == LidPositionStatus::Transitioning,
               "stable position between endpoints remains TRANSITIONING");

        snapshot = tracker.update(true, true, 22U);
        snapshot = tracker.update(true, true, 32U);
        expect(snapshot.position == LidPositionStatus::Open,
               "open endpoint becomes confirmed after its debounce interval");
    }

    {
        LidLimitSwitchTracker tracker(configured_pair());
        tracker.update(true, false, 0U);
        auto snapshot = tracker.update(true, false, 10U);
        expect(snapshot.position == LidPositionStatus::Unknown,
               "simultaneously active open and closed limits are contradictory");
    }

    {
        LidLimitSwitchConfig partial{
            {true, LimitSwitchActiveLevel::High, 10U},
            {false, LimitSwitchActiveLevel::Unspecified, 0U}
        };
        LidLimitSwitchTracker tracker(partial);
        const auto snapshot = tracker.update(true, false, 0U);
        expect(snapshot.position == LidPositionStatus::Unknown,
               "one unconfigured endpoint is not enough to verify lid position");
    }

    {
        LidLimitSwitchConfig invalid{
            {true, LimitSwitchActiveLevel::Unspecified, 10U},
            {true, LimitSwitchActiveLevel::Low, 0U}
        };
        LidLimitSwitchTracker tracker(invalid);
        const auto snapshot = tracker.update(true, false, 0U);
        expect(snapshot.open_limit == LimitSwitchSignalStatus::Unknown &&
                   snapshot.closed_limit == LimitSwitchSignalStatus::Unknown &&
                   snapshot.position == LidPositionStatus::Unknown,
               "missing polarity or zero debounce interval fails closed as UNKNOWN");
    }

    {
        LidLimitSwitchTracker tracker(configured_pair());
        tracker.update(true, true, 0U);
        auto snapshot = tracker.update(std::nullopt, true, 10U);
        expect(snapshot.open_limit == LimitSwitchSignalStatus::Unknown &&
                   snapshot.position == LidPositionStatus::Unknown,
               "unavailable raw input is UNKNOWN and clears its debounce history");

        snapshot = tracker.update(true, true, 11U);
        expect(snapshot.position == LidPositionStatus::Transitioning,
               "valid input must pass a fresh debounce interval after recovery");
    }

    {
        LidLimitSwitchTracker tracker(configured_pair());
        tracker.update(true, true, 100U);
        const auto backwards = tracker.update(true, true, 90U);
        expect(backwards.update_status ==
                   LidLimitSwitchUpdateStatus::MonotonicTimeWentBackwards &&
                   backwards.position == LidPositionStatus::Unknown,
               "monotonic clock regression resets history and reports UNKNOWN");

        auto snapshot = tracker.update(true, true, 91U);
        expect(snapshot.position == LidPositionStatus::Transitioning,
               "debounce restarts after a monotonic clock regression");
    }

    {
        LidLimitSwitchTracker tracker(configured_pair());
        tracker.update(true, true, 0U);
        tracker.configure(configured_pair(20U));
        auto snapshot = tracker.update(true, true, 100U);
        expect(snapshot.position == LidPositionStatus::Transitioning,
               "reconfiguration discards the previous debounce history");

        snapshot = tracker.update(true, true, 120U);
        expect(snapshot.position == LidPositionStatus::Open,
               "new debounce configuration is applied after a fresh interval");
    }

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
