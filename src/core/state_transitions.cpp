#include "state_transitions.hpp"
namespace dockstation::core {
namespace {
constexpr TransitionResult allowed(StationState next_state) noexcept {
    return {TransitionStatus::Allowed, next_state, {}, "Transition is allowed"};
}
constexpr TransitionResult rejected(std::string_view code,
                                     std::string_view text) noexcept {
    return {TransitionStatus::Rejected, std::nullopt, code, text};
}
}  // namespace
TransitionResult resolve_transition(
    StationState current_state,
    const Event& event,
    const TransitionContext& context) noexcept {
    // ВЫКЛЮЧЕНА -> ЗАПУСК: запуск инициируется стартовым программным слоем.
    if (current_state == StationState::PoweredOff &&
        event.event_type == "STARTUP_REQUESTED") {
        return allowed(StationState::Startup);
    }
    // ЗАПУСК -> САМОПРОВЕРКА: нельзя продолжать без проверенной конфигурации.
    if (current_state == StationState::Startup &&
        event.event_type == "INITIALIZATION_COMPLETED") {
        if (!context.configuration_validated) {
            return rejected("CONFIGURATION_NOT_VALIDATED",
                            "Stored configuration has not been positively validated");
        }
        return allowed(StationState::SelfTest);
    }
    // САМОПРОВЕРКА -> ОЖИДАНИЕ: нужны полный положительный отчёт,
    // подтверждённое безопасное исходное состояние и отсутствие восстановления.
    if (current_state == StationState::SelfTest &&
        event.event_type == "SELF_TEST_COMPLETED") {
        if (!context.diagnostic_report_complete) {
            return rejected("DIAGNOSTIC_REPORT_INCOMPLETE",
                            "The complete diagnostic report has not been received");
        }
        if (!context.critical_checks_passed) {
            return rejected("CRITICAL_CHECKS_NOT_PASSED",
                            "One or more critical checks are not positively passed");
        }
        if (!context.safe_initial_condition_confirmed) {
            return rejected("SAFE_INITIAL_CONDITION_UNCONFIRMED",
                            "The safe initial station condition is not confirmed");
        }
        // UNKNOWN и REQUIRED одинаково блокируют переход: восстановление должно быть
        // явно исключено до перехода в ОЖИДАНИЕ.
        if (context.recovery_status != RecoveryStatus::NotRequired) {
            return rejected("RECOVERY_REQUIRED",
                            "Recovery and equipment reconciliation are required before waiting");
        }
        return allowed(StationState::Waiting);
    }
    // Все остальные пары state/event пока не входят в согласованный первый срез.
    return rejected("TRANSITION_NOT_APPROVED",
                    "No approved transition exists for this state and event pair");
}
}  // namespace dockstation::core
