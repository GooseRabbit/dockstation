#pragma once

#include <cstdint>

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

}  // namespace dockstation::core
