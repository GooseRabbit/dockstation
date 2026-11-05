#pragma once
#include <optional>
#include <string_view>
#include "event.hpp"
namespace dockstation::core {
// Полный перечень состояний взят из docs/04_OPERATIONAL_ALGORITHMS.md.
// В этой реализации разрешены только три перехода первого согласованного среза.
enum class StationState {
    PoweredOff, Startup, SelfTest, Waiting, Detection, Authentication,
    Authorization, ReceptionPreparation, LidOpening, LandingGuidance,
    LandingConfirmation, LidClosing, EnclosedService, MissionWaiting,
    TakeoffPreparation, OpeningForTakeoff, Takeoff, SelfCleaning,
    LimitedMode, AwaitingIntervention, Recovery, Emergency,
};
enum class TransitionStatus { Allowed, Rejected };
// Входные данные: подтверждения от координаторов запуска, конфигурации и диагностики.
// true устанавливается только после достоверно положительной проверки;
// отсутствие результата, UNKNOWN и ошибка должны оставаться false.
struct TransitionContext {
    bool configuration_validated{false};
    bool diagnostic_report_complete{false};
    bool critical_checks_passed{false};
    bool safe_initial_condition_confirmed{false};
    bool recovery_required{false};
};
struct TransitionResult {
    TransitionStatus status{TransitionStatus::Rejected};
    std::optional<StationState> next_state{};
    std::string_view reason_code{};
    std::string_view reason_text{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == TransitionStatus::Allowed;
    }
};
// Чистая функция выбора одного из трёх согласованных переходов запуска.
// Вход: текущее глобальное состояние, проверенное событие и подтверждения условий.
// Предусловие: событие уже прошло validate_event(); функция не обращается к оборудованию,
// хранилищу, часам или шине событий и не выполняет побочных действий.
[[nodiscard]] TransitionResult resolve_transition(
    StationState current_state,
    const Event& event,
    const TransitionContext& context = {}) noexcept;
}  // namespace dockstation::core
