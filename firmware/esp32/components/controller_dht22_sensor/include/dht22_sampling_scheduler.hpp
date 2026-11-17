#pragma once

#include <cstdint>

namespace dockstation::firmware::esp32 {

enum class Dht22SamplingStatus : std::uint8_t {
    Ready,
    NotInitialized,
    PowerUpDelay,
    MinimumInterval,
    MonotonicTimeWentBackwards
};

struct Dht22SamplingDecision final {
    Dht22SamplingStatus status{Dht22SamplingStatus::NotInitialized};
    std::uint64_t remaining_wait_ms{0U};
};

// Pure sampling policy for one DHT22/AM2302 sensor.
// Initialize with the monotonic timestamp at which sensor power was applied.
// A first read is allowed only after more than 2 seconds; subsequent read
// attempts are separated by at least 2 seconds. Each instance tracks one
// physical sensor, so two sensors use two independent instances.
class Dht22SamplingScheduler final {
public:
    static constexpr std::uint64_t kPowerUpWaitMs = 2000U;
    static constexpr std::uint64_t kMinimumReadIntervalMs = 2000U;

    Dht22SamplingScheduler() noexcept = default;

    // Call when the sensor is powered on, using the caller's monotonic clock.
    // Reinitialization discards the prior scheduling history.
    void initialize(std::uint64_t powered_on_at_ms) noexcept;

    // Call immediately before starting a sensor read. A Ready result consumes
    // the slot, so a failed acquisition attempt is also rate-limited.
    // A backwards clock is fail-closed until initialize() is called again.
    [[nodiscard]] Dht22SamplingDecision try_begin_read(
        std::uint64_t monotonic_time_ms) noexcept;

private:
    bool initialized_{false};
    bool has_last_read_{false};
    std::uint64_t powered_on_at_ms_{0U};
    std::uint64_t last_read_started_at_ms_{0U};
    std::uint64_t last_observed_at_ms_{0U};
};

}  // namespace dockstation::firmware::esp32
