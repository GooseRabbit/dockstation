#include "recovery.hpp"

#include <string_view>

namespace dockstation::core {
namespace {

bool has_non_whitespace(std::string_view value) noexcept {
    for (const char ch : value) {
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' &&
            ch != '\f' && ch != '\v') {
            return true;
        }
    }
    return false;
}

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
    return has_non_whitespace(snapshot.transition_id);
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



namespace {

bool is_known_recovery_quality(RecoveryDataQuality quality) noexcept {
    switch (quality) {
        case RecoveryDataQuality::Valid:
        case RecoveryDataQuality::Stale:
        case RecoveryDataQuality::Invalid:
        case RecoveryDataQuality::Unavailable:
        case RecoveryDataQuality::Unknown:
        case RecoveryDataQuality::Error:
            return true;
    }
    return false;
}

bool is_unfinished_lifecycle(OperationLifecycleStatus status) noexcept {
    switch (status) {
        case OperationLifecycleStatus::Requested:
        case OperationLifecycleStatus::Accepted:
        case OperationLifecycleStatus::Started:
        case OperationLifecycleStatus::CancelRequested:
        case OperationLifecycleStatus::OutcomeUnknown:
            return true;
        case OperationLifecycleStatus::Completed:
        case OperationLifecycleStatus::Failed:
        case OperationLifecycleStatus::Cancelled:
            return false;
    }
    return false;
}

bool is_known_recovery_outcome(RecoveryOperationOutcome outcome) noexcept {
    switch (outcome) {
        case RecoveryOperationOutcome::NotExecuted:
        case RecoveryOperationOutcome::InProgress:
        case RecoveryOperationOutcome::Completed:
        case RecoveryOperationOutcome::Failed:
        case RecoveryOperationOutcome::Cancelled:
        case RecoveryOperationOutcome::Unknown:
            return true;
    }
    return false;
}

bool is_known_collection_status(RecoveryCollectionStatus status) noexcept {
    return status == RecoveryCollectionStatus::Complete ||
           status == RecoveryCollectionStatus::Incomplete ||
           status == RecoveryCollectionStatus::Error;
}

}  // namespace

RecoveryReconciliationResult reconcile_after_restart(
    const StationStateSnapshot& snapshot,
    const RecoveryReports& reports) noexcept {

    // Проверяем только целостность исторического snapshot; он не доказывает текущее положение механизмов.
    if (!is_valid_snapshot(snapshot)) {
        return {RecoveryReconciliationStatus::InvalidInput, 0, 0};
    }

    // Нельзя считать некорректный указатель пустым перечнем отчётов.
    if ((reports.subsystem_report_count > 0 && reports.subsystem_reports == nullptr) ||
        (reports.unfinished_operation_count > 0 && reports.unfinished_operations == nullptr) ||
        (reports.operation_report_count > 0 && reports.operation_reports == nullptr) ||
        !is_known_collection_status(reports.subsystem_collection) ||
        !is_known_collection_status(reports.operation_collection)) {
        return {RecoveryReconciliationStatus::InvalidInput, 0, 0};
    }

    bool conflict = false;
    bool incomplete = false;
    bool unsafe_subsystem = false;
    bool operation_in_progress = false;
    std::size_t checked_subsystems = 0;
    std::size_t checked_operations = 0;

    // Complete означает, что адаптер включил все применимые сводные отчёты.
    if (reports.subsystem_collection != RecoveryCollectionStatus::Complete ||
        reports.subsystem_report_count == 0) {
        incomplete = true;
    }

    // Проверяем уникальность, качество и нормализованное состояние каждой подсистемы.
    for (std::size_t i = 0; i < reports.subsystem_report_count; ++i) {
        const RecoverySubsystemReport& report = reports.subsystem_reports[i];
        if (!has_non_whitespace(report.subsystem_id) ||
            !is_known_recovery_quality(report.quality)) {
            return {RecoveryReconciliationStatus::InvalidInput,
                    checked_subsystems, checked_operations};
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (reports.subsystem_reports[j].subsystem_id == report.subsystem_id) {
                conflict = true;
            }
        }
        if (report.quality != RecoveryDataQuality::Valid) {
            incomplete = true;
        }
        switch (report.condition) {
            case RecoverySubsystemCondition::Safe:
                break;
            case RecoverySubsystemCondition::Unsafe:
                // Только актуальный достоверный отчёт позволяет назвать подсистему небезопасной.
                if (report.quality == RecoveryDataQuality::Valid) {
                    unsafe_subsystem = true;
                } else {
                    incomplete = true;
                }
                break;
            case RecoverySubsystemCondition::Unknown:
                incomplete = true;
                break;
            default:
                return {RecoveryReconciliationStatus::InvalidInput,
                        checked_subsystems, checked_operations};
        }
        ++checked_subsystems;
    }

    // Адаптер обязан предоставить полный список незавершённых операций и отчёты для него.
    if (reports.operation_collection != RecoveryCollectionStatus::Complete) {
        incomplete = true;
    }
    if (reports.operation_report_count > reports.unfinished_operation_count) {
        conflict = true; // Получен отчёт об операции, отсутствующей в полном сохранённом списке.
    } else if (reports.operation_report_count < reports.unfinished_operation_count) {
        incomplete = true; // Не для каждой незавершённой операции получен отчёт.
    }

    for (std::size_t i = 0; i < reports.unfinished_operation_count; ++i) {
        const OperationRecord& saved = reports.unfinished_operations[i];
        if (!has_non_whitespace(saved.request.operation_id) ||
            !has_non_whitespace(saved.request.correlation_id) ||
            !has_non_whitespace(saved.request.target_module) ||
            !has_non_whitespace(saved.request.action) ||
            saved.revision == 0U ||
            !is_unfinished_lifecycle(saved.status)) {
            return {RecoveryReconciliationStatus::InvalidInput,
                    checked_subsystems, checked_operations};
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (reports.unfinished_operations[j].request.operation_id ==
                saved.request.operation_id) {
                conflict = true;
            }
        }

        std::size_t matching_reports = 0;
        for (std::size_t j = 0; j < reports.operation_report_count; ++j) {
            const RecoveryOperationReport& report = reports.operation_reports[j];
            if (report.operation_id == saved.request.operation_id) {
                ++matching_reports;
                if (report.correlation_id != saved.request.correlation_id) {
                    conflict = true;
                }
            }
        }
        if (matching_reports == 0) {
            incomplete = true;
        } else if (matching_reports > 1) {
            conflict = true;
        } else {
            ++checked_operations;
        }
    }

    // Проверяем каждый отчёт исполнителя; неизвестный или устаревший результат не закрывает операцию.
    for (std::size_t i = 0; i < reports.operation_report_count; ++i) {
        const RecoveryOperationReport& report = reports.operation_reports[i];
        if (!has_non_whitespace(report.operation_id) ||
            !has_non_whitespace(report.correlation_id) ||
            !is_known_recovery_quality(report.quality) ||
            !is_known_recovery_outcome(report.outcome)) {
            return {RecoveryReconciliationStatus::InvalidInput,
                    checked_subsystems, checked_operations};
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (reports.operation_reports[j].operation_id == report.operation_id) {
                conflict = true;
            }
        }
        bool matches_saved_operation = false;
        for (std::size_t j = 0; j < reports.unfinished_operation_count; ++j) {
            if (reports.unfinished_operations[j].request.operation_id == report.operation_id) {
                matches_saved_operation = true;
                break;
            }
        }
        if (!matches_saved_operation) {
            conflict = true; // Отчёт нельзя сопоставить ни с одной сохранённой незавершённой операцией.
        }
        if (report.quality != RecoveryDataQuality::Valid ||
            report.outcome == RecoveryOperationOutcome::Unknown) {
            incomplete = true;
        }
        if (report.quality == RecoveryDataQuality::Valid &&
            report.outcome == RecoveryOperationOutcome::InProgress) {
            operation_in_progress = true;
        }

        // Сохранённый STARTED противоречит достоверному отчёту, что операция не выполнялась.
        for (std::size_t j = 0; j < reports.unfinished_operation_count; ++j) {
            const OperationRecord& saved = reports.unfinished_operations[j];
            if (saved.request.operation_id == report.operation_id &&
                saved.status == OperationLifecycleStatus::Started &&
                report.quality == RecoveryDataQuality::Valid &&
                report.outcome == RecoveryOperationOutcome::NotExecuted) {
                conflict = true;
            }
        }
    }

    // Конфликт имеет приоритет над частичными результатами; небезопасность не теряется за UNKNOWN.
    if (conflict) {
        return {RecoveryReconciliationStatus::Conflict,
                checked_subsystems, checked_operations};
    }
    if (unsafe_subsystem) {
        return {RecoveryReconciliationStatus::SubsystemUnsafe,
                checked_subsystems, checked_operations};
    }
    if (operation_in_progress) {
        return {RecoveryReconciliationStatus::OperationInProgress,
                checked_subsystems, checked_operations};
    }
    if (incomplete) {
        return {RecoveryReconciliationStatus::Incomplete,
                checked_subsystems, checked_operations};
    }
    return {RecoveryReconciliationStatus::EvidenceComplete,
            checked_subsystems, checked_operations};
}

}  // namespace dockstation::core
