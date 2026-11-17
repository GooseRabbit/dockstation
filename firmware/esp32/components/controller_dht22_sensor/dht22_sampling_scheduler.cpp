#include "dht22_sampling_scheduler.hpp"

namespace dockstation::firmware::esp32 {

void Dht22SamplingScheduler::initialize(
    std::uint64_t powered_on_at_ms) noexcept {
    initialized_ = true;
    has_last_read_ = false;
    powered_on_at_ms_ = powered_on_at_ms;
    last_read_started_at_ms_ = 0U;
    last_observed_at_ms_ = powered_on_at_ms;
}

Dht22SamplingDecision Dht22SamplingScheduler::try_begin_read(
    std::uint64_t monotonic_time_ms) noexcept {
    if (!initialized_) {
        return {Dht22SamplingStatus::NotInitialized, 0U};
    }

    if (monotonic_time_ms < last_observed_at_ms_) {
        initialized_ = false;
        has_last_read_ = false;
        return {Dht22SamplingStatus::MonotonicTimeWentBackwards, 0U};
    }

    last_observed_at_ms_ = monotonic_time_ms;

    if (!has_last_read_) {
        const std::uint64_t elapsed_ms =
            monotonic_time_ms - powered_on_at_ms_;
        if (elapsed_ms <= kPowerUpWaitMs) {
            return {
                Dht22SamplingStatus::PowerUpDelay,
                (kPowerUpWaitMs + 1U) - elapsed_ms
            };
        }
    } else {
        const std::uint64_t elapsed_ms =
            monotonic_time_ms - last_read_started_at_ms_;
        if (elapsed_ms < kMinimumReadIntervalMs) {
            return {
                Dht22SamplingStatus::MinimumInterval,
                kMinimumReadIntervalMs - elapsed_ms
            };
        }
    }

    // Reserve the slot before the caller starts acquisition. This also
    // prevents tight retries when the physical read itself fails.
    has_last_read_ = true;
    last_read_started_at_ms_ = monotonic_time_ms;
    return {Dht22SamplingStatus::Ready, 0U};
}

}  // namespace dockstation::firmware::esp32
