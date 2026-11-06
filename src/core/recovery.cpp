#include "recovery.hpp"

namespace dockstation::core {
namespace {

bool is_known_state(StationState state) noexcept {
    switch (state) {
        case StationState::PoweredOff:
        case StationState::Startup:
        case StationState::SelfTest:
        case StationState::Waiting:
        case StationState::Detection:
        case StationState::Authentication:
        case StationState::Authorization:
        case StationState::ReceptionPreparation:
        case StationState::LidOpening:
        case StationState::LandingGuidance:
        case StationState::LandingConfirmation:
        case StationState::LidClosing:
        case StationState::EnclosedService:
        case StationState::MissionWaiting:
        case StationState::TakeoffPreparation:
        case StationState::OpeningForTakeoff:
        case StationState::Takeoff:
        case StationState::SelfCleaning:
        case StationState::LimitedMode:
        case StationState::AwaitingIntervention:
        case StationState::Recovery:
        case StationState::Emergency:
            return true;
    }
    return false;
}

bool is_valid_snapshot(const StationStateSnapshot& snapshot) noexcept {
    // Номер версии 0 обозначает исходный snapshot без подтверждённого перехода.
    // Любой положительный revision должен ссылаться на последний transition_id.
    if (!is_known_state(snapshot.state)) {
        return false;
    }
    if (snapshot.revision == 0) {
        return snapshot.transition_id.empty();
    }
    return !snapshot.transition_id.empty();
}

}  // namespace

RecoveryStartResult begin_recovery(
    RecoveryRepository& repository,                  // Постоянное хранилище состояния и маркера восстановления.
    RecoveryReportRequester& report_requester) noexcept { // Адаптер запроса свежих отчётов подсистем.

    // Сначала загружаем последний подтверждённый snapshot; отсутствующие данные не заменяем догадками.
    const RecoverySnapshotLoadResult loaded = repository.load_snapshot();
    switch (loaded.status) {
        case RecoverySnapshotLoadStatus::NotFound:
            return {RecoveryStartStatus::SnapshotNotFound, false, {}};
        case RecoverySnapshotLoadStatus::Error:
            return {RecoveryStartStatus::RepositoryError, false, {}};
        case RecoverySnapshotLoadStatus::Found:
            break;
        default:
            return {RecoveryStartStatus::RepositoryError, false, {}};
    }

    // Не позволяем повреждённому или противоречивому snapshot запускать восстановление.
    if (!is_valid_snapshot(loaded.snapshot)) {
        return {RecoveryStartStatus::InvalidSnapshot, true, loaded.snapshot};
    }

    // До опроса подсистем сохраняем факт начала восстановления.
    // Репозиторий сравнивает ожидаемые state/revision, чтобы не затереть более новое состояние.
    const RecoveryMarkStatus marked = repository.mark_recovery_in_progress(loaded.snapshot);
    switch (marked) {
        case RecoveryMarkStatus::Marked:
        case RecoveryMarkStatus::AlreadyInProgress:
            break;
        case RecoveryMarkStatus::SnapshotChanged:
            return {RecoveryStartStatus::SnapshotChanged, true, loaded.snapshot};
        case RecoveryMarkStatus::Error:
            return {RecoveryStartStatus::RepositoryError, true, loaded.snapshot};
        default:
            return {RecoveryStartStatus::RepositoryError, true, loaded.snapshot};
    }

    // Только после устойчивого маркера запрашиваем отчёты; это запрос данных, не физическая команда.
    const RecoveryReportRequestStatus requested =
        report_requester.request_fresh_reports(loaded.snapshot);
    switch (requested) {
        case RecoveryReportRequestStatus::Accepted:
            return {marked == RecoveryMarkStatus::Marked
                        ? RecoveryStartStatus::Started
                        : RecoveryStartStatus::AlreadyInProgress,
                    true, loaded.snapshot};
        case RecoveryReportRequestStatus::AlreadyPending:
            return {RecoveryStartStatus::AlreadyInProgress, true, loaded.snapshot};
        case RecoveryReportRequestStatus::Rejected:
            return {RecoveryStartStatus::ReportRequestRejected, true, loaded.snapshot};
        case RecoveryReportRequestStatus::Unknown:
            return {RecoveryStartStatus::ReportRequestUnknown, true, loaded.snapshot};
        case RecoveryReportRequestStatus::Error:
            return {RecoveryStartStatus::ReportRequestError, true, loaded.snapshot};
    }

    // Неизвестный ответ внешнего порта не считается подтверждённым запросом.
    return {RecoveryStartStatus::ReportRequestError, true, loaded.snapshot};
}

}  // namespace dockstation::core
