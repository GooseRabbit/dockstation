#include "controller_link_reconnect_backoff.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

using dockstation::firmware::esp32::ControllerLinkReconnectBackoff;

namespace {
int checks = 0;
int failures = 0;

void expect(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << description << std::endl;
    } else {
        std::cerr << "FAIL - " << description << std::endl;
        ++failures;
    }
}
}  // namespace

int main() {
    ControllerLinkReconnectBackoff backoff;
    expect(!backoff.is_scheduled() && !backoff.retry_due(100000U),
           "no retry is pending before a failure");
    expect(backoff.schedule_after_failure(100U) == 1000U &&
               backoff.scheduled_delay_ms() == 1000U &&
               !backoff.retry_due(1099U) && backoff.retry_due(1100U),
           "first retry uses a one-second delay and the exact deadline");
    backoff.begin_attempt();
    expect(!backoff.is_scheduled() && !backoff.retry_due(9000U),
           "starting an attempt consumes the pending retry");
    expect(backoff.schedule_after_failure(2000U) == 2000U &&
               !backoff.retry_due(3999U) && backoff.retry_due(4000U),
           "second consecutive failure doubles the delay to two seconds");
    backoff.begin_attempt();
    expect(backoff.schedule_after_failure(5000U) == 4000U,
           "third consecutive failure doubles the delay to four seconds");
    constexpr std::array<std::uint32_t, 4U> expected{8000U, 16000U, 30000U, 30000U};
    for (const std::uint32_t delay : expected) {
        backoff.begin_attempt();
        expect(backoff.schedule_after_failure(6000U) == delay,
               "exponential delay advances and remains capped at thirty seconds");
    }
    expect(!backoff.retry_due(5999U),
           "a monotonic clock regression never triggers an early retry");
    expect(backoff.retry_due(36000U),
           "the maximum delay becomes due at its exact boundary");
    backoff.reset();
    expect(!backoff.is_scheduled() &&
               backoff.schedule_after_failure(50000U) == 1000U,
           "a successful session resets the next retry to one second");
    std::cout << "Checks: " << checks << ", failures: " << failures << std::endl;
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
