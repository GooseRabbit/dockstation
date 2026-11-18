#pragma once
#include <cstddef>
#include <cstdint>
namespace dockstation::firmware::esp32 {
inline constexpr std::size_t kControllerUartMaximumEventsPerPoll = 8U;
enum class ControllerUartEventKind : std::uint8_t { Data, Informational, FifoOverflow, ReceiveBufferFull, FrameError, ParityError, BreakDetected };
enum class ControllerUartEventFault : std::uint8_t { None, FifoOverflow, ReceiveBufferFull, FrameError, ParityError, BreakDetected };
enum class ControllerUartEventDrainStatus : std::uint8_t { Drained, MorePending, Fault };
struct ControllerUartEventDrainResult final {
    ControllerUartEventDrainStatus status{ControllerUartEventDrainStatus::Drained};
    std::size_t processed_events{0U};
    ControllerUartEventFault fault{ControllerUartEventFault::None};
};
[[nodiscard]] constexpr ControllerUartEventFault controller_uart_event_fault(ControllerUartEventKind kind) noexcept {
    switch (kind) {
        case ControllerUartEventKind::FifoOverflow: return ControllerUartEventFault::FifoOverflow;
        case ControllerUartEventKind::ReceiveBufferFull: return ControllerUartEventFault::ReceiveBufferFull;
        case ControllerUartEventKind::FrameError: return ControllerUartEventFault::FrameError;
        case ControllerUartEventKind::ParityError: return ControllerUartEventFault::ParityError;
        case ControllerUartEventKind::BreakDetected: return ControllerUartEventFault::BreakDetected;
        case ControllerUartEventKind::Data:
        case ControllerUartEventKind::Informational: return ControllerUartEventFault::None;
    }
    return ControllerUartEventFault::BreakDetected;
}
template <typename TryReceiveEvent, typename HasPendingEvents>
[[nodiscard]] ControllerUartEventDrainResult drain_controller_uart_event_batch(
    TryReceiveEvent&& try_receive_event, HasPendingEvents&& has_pending_events) noexcept {
    ControllerUartEventDrainResult result{};
    for (std::size_t count = 0U; count < kControllerUartMaximumEventsPerPoll; ++count) {
        ControllerUartEventKind kind{ControllerUartEventKind::Informational};
        if (!try_receive_event(kind)) return result;
        ++result.processed_events;
        result.fault = controller_uart_event_fault(kind);
        if (result.fault != ControllerUartEventFault::None) {
            result.status = ControllerUartEventDrainStatus::Fault;
            return result;
        }
    }
    result.status = has_pending_events() ? ControllerUartEventDrainStatus::MorePending
                                         : ControllerUartEventDrainStatus::Drained;
    return result;
}
}  // namespace dockstation::firmware::esp32
