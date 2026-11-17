#include "dht22_sampling_scheduler.hpp"

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
}  // namespace

int main() {
    {
        Dht22SamplingScheduler scheduler;
        const auto decision = scheduler.try_begin_read(1000U);
        expect(decision.status == Dht22SamplingStatus::NotInitialized,
               "scheduler requires a known sensor power-on timestamp");
    }

    {
        Dht22SamplingScheduler scheduler;
        scheduler.initialize(1000U);

        auto decision = scheduler.try_begin_read(1000U);
        expect(decision.status == Dht22SamplingStatus::PowerUpDelay &&
                   decision.remaining_wait_ms == 2001U,
               "power-up delay starts from the caller-supplied power timestamp");

        decision = scheduler.try_begin_read(3000U);
        expect(decision.status == Dht22SamplingStatus::PowerUpDelay &&
                   decision.remaining_wait_ms == 1U,
               "first read is still blocked at exactly two seconds");

        decision = scheduler.try_begin_read(3001U);
        expect(decision.status == Dht22SamplingStatus::Ready &&
                   decision.remaining_wait_ms == 0U,
               "first read is allowed only after more than two seconds");
    }

    {
        Dht22SamplingScheduler scheduler;
        scheduler.initialize(0U);

        auto decision = scheduler.try_begin_read(2001U);
        expect(decision.status == Dht22SamplingStatus::Ready,
               "first sample is permitted after the startup wait");

        decision = scheduler.try_begin_read(2001U);
        expect(decision.status == Dht22SamplingStatus::MinimumInterval &&
                   decision.remaining_wait_ms == 2000U,
               "a second read at the same time is blocked");

        decision = scheduler.try_begin_read(3999U);
        expect(decision.status == Dht22SamplingStatus::MinimumInterval &&
                   decision.remaining_wait_ms == 2U,
               "minimum interval is not shortened by one millisecond");

        decision = scheduler.try_begin_read(4000U);
        expect(decision.status == Dht22SamplingStatus::MinimumInterval &&
                   decision.remaining_wait_ms == 1U,
               "read remains blocked one millisecond before the interval ends");

        decision = scheduler.try_begin_read(4001U);
        expect(decision.status == Dht22SamplingStatus::Ready,
               "next read is allowed at the exact two-second boundary");

        decision = scheduler.try_begin_read(6001U);
        expect(decision.status == Dht22SamplingStatus::Ready,
               "later reads remain separated by at least two seconds");
    }

    {
        Dht22SamplingScheduler first_sensor;
        Dht22SamplingScheduler second_sensor;
        first_sensor.initialize(0U);
        second_sensor.initialize(500U);

        const auto first = first_sensor.try_begin_read(2001U);
        const auto second = second_sensor.try_begin_read(2001U);
        expect(first.status == Dht22SamplingStatus::Ready &&
                   second.status == Dht22SamplingStatus::PowerUpDelay &&
                   second.remaining_wait_ms == 500U,
               "two sensors keep independent power-up schedules");
    }

    {
        Dht22SamplingScheduler scheduler;
        scheduler.initialize(100U);
        const auto ready = scheduler.try_begin_read(2101U);
        const auto backwards = scheduler.try_begin_read(2100U);
        const auto latched = scheduler.try_begin_read(2200U);

        expect(ready.status == Dht22SamplingStatus::Ready &&
                   backwards.status ==
                       Dht22SamplingStatus::MonotonicTimeWentBackwards &&
                   latched.status == Dht22SamplingStatus::NotInitialized,
               "clock regression blocks further reads until explicit reinitialization");
    }

    {
        Dht22SamplingScheduler scheduler;
        scheduler.initialize(0U);
        const auto first = scheduler.try_begin_read(2001U);
        scheduler.initialize(10000U);
        const auto after_power_cycle = scheduler.try_begin_read(12000U);

        expect(first.status == Dht22SamplingStatus::Ready &&
                   after_power_cycle.status == Dht22SamplingStatus::PowerUpDelay &&
                   after_power_cycle.remaining_wait_ms == 1U,
               "reinitialization clears the old schedule for a new power cycle");
    }

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
