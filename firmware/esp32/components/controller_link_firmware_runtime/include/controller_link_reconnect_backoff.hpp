#pragma once

#include <cstdint>

namespace dockstation::firmware::esp32 {

class ControllerLinkReconnectBackoff final {
public:
    static constexpr std::uint32_t kInitialDelayMilliseconds = 1000U;
    static constexpr std::uint32_t kMaximumDelayMilliseconds = 30000U;

    [[nodiscard]] std::uint32_t schedule_after_failure(std::uint64_t now_ms) noexcept {
        scheduled_at_ms_ = now_ms;
        scheduled_delay_ms_ = next_delay_ms_;
        retry_scheduled_ = true;
        if (next_delay_ms_ >= (kMaximumDelayMilliseconds / 2U)) {
            next_delay_ms_ = kMaximumDelayMilliseconds;
        } else {
            next_delay_ms_ *= 2U;
        }
        return scheduled_delay_ms_;
    }

    [[nodiscard]] bool retry_due(std::uint64_t now_ms) const noexcept {
        return retry_scheduled_ && now_ms >= scheduled_at_ms_ &&
               (now_ms - scheduled_at_ms_) >= scheduled_delay_ms_;
    }

    void begin_attempt() noexcept {
        retry_scheduled_ = false;
        scheduled_delay_ms_ = 0U;
    }

    void reset() noexcept {
        retry_scheduled_ = false;
        scheduled_at_ms_ = 0U;
        scheduled_delay_ms_ = 0U;
        next_delay_ms_ = kInitialDelayMilliseconds;
    }

    [[nodiscard]] bool is_scheduled() const noexcept { return retry_scheduled_; }
    [[nodiscard]] std::uint32_t scheduled_delay_ms() const noexcept {
        return scheduled_delay_ms_;
    }

private:
    std::uint64_t scheduled_at_ms_{0U};
    std::uint32_t scheduled_delay_ms_{0U};
    std::uint32_t next_delay_ms_{kInitialDelayMilliseconds};
    bool retry_scheduled_{false};
};

}  // namespace dockstation::firmware::esp32
