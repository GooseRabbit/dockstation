#pragma once

#include <cstddef>
#include <cstdint>

#include "operations.hpp"

#include "state_transitions.hpp"

namespace dockstation::core {

// Результат чтения последнего подтверждённого снимка станции.
enum class RecoverySnapshotLoadStatus : std::uint8_t {
    Found,     // Последний сохранённый snapshot найден и возвращён.
    NotFound,  // Сохранённого snapshot нет; восстанавливать состояние по догадке нельзя.
    Error      // Хранилище не подтвердило результат чтения.
};

struct RecoverySnapshotLoadResult {
    RecoverySnapshotLoadStatus status{RecoverySnapshotLoadStatus::Error}; // Итог чтения snapshot.
    StationStateSnapshot snapshot{}; // Последний подтверждённый снимок; используется как историческая база, не как факт о датчиках.
};

enum class RecoveryMarkStatus : std::uint8_t {
    Marked,             // Признак RECOVERY_IN_PROGRESS устойчиво сохранён впервые.
    AlreadyInProgress,  // Признак уже сохранён; повторная установка не создаёт новый запуск.
    SnapshotChanged,    // Сохранённые state/revision отличаются от ожидаемого snapshot.
    Error               // Устойчивое сохранение признака не подтверждено.
};

// Логический порт постоянного хранилища для начала восстановления.
// Может быть реализован тем же физическим адаптером, что и другие repository-порты;
// отдельная база данных этим интерфейсом не подразумевается.
class RecoveryRepository {
public:
    virtual ~RecoveryRepository() = default;

    // Загружает последний подтверждённый глобальный snapshot.
    // transition_id — невладеющая строка; память должна оставаться действительной
    // до завершения F14 и последующей передачи snapshot в F15.
    [[nodiscard]] virtual RecoverySnapshotLoadResult load_snapshot() noexcept = 0;

    // Атомарно сверяет state/revision с загруженным snapshot и устанавливает
    // постоянный признак RECOVERY_IN_PROGRESS. Точный повтор должен быть идемпотентным.
    [[nodiscard]] virtual RecoveryMarkStatus mark_recovery_in_progress(
        const StationStateSnapshot& expected_snapshot) noexcept = 0;
};

enum class RecoveryReportRequestStatus : std::uint8_t {
    Accepted,      // Запрос свежих отчётов принят; сами отчёты ещё не получены.
    AlreadyPending,// Такой запрос уже ожидает выполнения.
    Rejected,      // Адаптер достоверно отклонил запрос.
    Unknown,       // Нельзя определить, был ли запрос принят.
    Error          // Ошибка запроса или обработки ответа.
};

// Логический порт запроса свежих отчётов у применимых подсистем.
class RecoveryReportRequester {
public:
    virtual ~RecoveryReportRequester() = default;

    // Только запрашивает данные; не должен запускать, останавливать или возобновлять
    // физические операции. Повтор для того же snapshot должен быть безопасен.
    [[nodiscard]] virtual RecoveryReportRequestStatus request_fresh_reports(
        const StationStateSnapshot& snapshot) noexcept = 0;
};

enum class RecoveryStartStatus : std::uint8_t {
    Started,               // Новый маркер сохранён, запрос отчётов принят.
    AlreadyInProgress,     // Восстановление уже шло; запрос отчётов принят или уже ожидает.
    SnapshotNotFound,      // Нет исходного snapshot.
    InvalidSnapshot,       // Snapshot содержит неизвестное/противоречивое состояние.
    SnapshotChanged,       // Snapshot изменился до сохранения маркера.
    RepositoryError,       // Ошибка чтения/устойчивой записи маркера.
    ReportRequestRejected, // Запрос отчётов отклонён.
    ReportRequestUnknown,  // Результат запроса отчётов не определён.
    ReportRequestError     // Ошибка или неизвестный код ответа запроса.
};

struct RecoveryStartResult {
    RecoveryStartStatus status{RecoveryStartStatus::RepositoryError}; // Результат запуска/инициации восстановления.
    bool has_snapshot{false}; // true, если исходный snapshot был загружен и возвращается вызывающему слою.
    StationStateSnapshot previous_snapshot{}; // Исторический snapshot для последующей F15; transition_id остаётся невладеющим.
};

// F14: загружает сохранённое состояние, фиксирует незавершённое восстановление
// и запрашивает свежие отчёты подсистем. Не сверяет отчёты (это F15), не меняет
// глобальное состояние и не возобновляет физические операции.
[[nodiscard]] RecoveryStartResult begin_recovery(
    RecoveryRepository& repository, // Чтение snapshot и устойчивое сохранение маркера восстановления.
    RecoveryReportRequester& report_requester) noexcept; // Только запрос свежих данных от подсистем.


// Нормализованное качество отчёта, определённое доверенным адаптером подсистемы.
enum class RecoveryDataQuality : std::uint8_t { Valid, Stale, Invalid, Unavailable, Unknown, Error };
// Состояние подсистемы после её собственной проверки; пороги и аппаратные детали здесь не задаются.
enum class RecoverySubsystemCondition : std::uint8_t { Safe, Unsafe, Unknown };

struct RecoverySubsystemReport {
    std::string_view subsystem_id{}; // Уникальный логический ID сводного отчёта подсистемы.
    RecoveryDataQuality quality{RecoveryDataQuality::Unknown}; // Достоверность и актуальность отчёта.
    RecoverySubsystemCondition condition{RecoverySubsystemCondition::Unknown}; // Нормализованный результат проверки.
};

// Фактический результат незавершённой операции, подтверждённый её исполнительным модулем.
enum class RecoveryOperationOutcome : std::uint8_t {
    NotExecuted, InProgress, Completed, Failed, Cancelled, Unknown
};

struct RecoveryOperationReport {
    std::string_view operation_id{}; // Должен совпасть с сохранённой записью операции.
    std::string_view correlation_id{}; // Защита от сопоставления с чужим запросом.
    RecoveryDataQuality quality{RecoveryDataQuality::Unknown}; // Качество свежего отчёта исполнителя.
    RecoveryOperationOutcome outcome{RecoveryOperationOutcome::Unknown}; // Фактический исход, не команда на повтор.
};

enum class RecoveryCollectionStatus : std::uint8_t { Complete, Incomplete, Error };

// Агрегат: все применимые сводные отчёты и полный список незавершённых операций.
// Массивы и все string_view внутри них должны оставаться действительными до возврата F15.
struct RecoveryReports {
    RecoveryCollectionStatus subsystem_collection{RecoveryCollectionStatus::Error};
    const RecoverySubsystemReport* subsystem_reports{nullptr};
    std::size_t subsystem_report_count{0};
    RecoveryCollectionStatus operation_collection{RecoveryCollectionStatus::Error};
    const OperationRecord* unfinished_operations{nullptr};
    std::size_t unfinished_operation_count{0};
    const RecoveryOperationReport* operation_reports{nullptr};
    std::size_t operation_report_count{0};
};

enum class RecoveryReconciliationStatus : std::uint8_t {
    EvidenceComplete, Incomplete, SubsystemUnsafe, OperationInProgress, Conflict, InvalidInput
};

struct RecoveryReconciliationResult {
    RecoveryReconciliationStatus status{RecoveryReconciliationStatus::InvalidInput};
    std::size_t checked_subsystem_count{0};
    std::size_t checked_operation_count{0};
};

// EvidenceComplete означает только полноту и непротиворечивость входных сведений.
// Это не подтверждение соответствия глобального state физике и не разрешение на действия.
// Функция не меняет state, не снимает RECOVERY_IN_PROGRESS и не возобновляет операции.
[[nodiscard]] RecoveryReconciliationResult reconcile_after_restart(
    const StationStateSnapshot& snapshot, const RecoveryReports& reports) noexcept;

}  // namespace dockstation::core
