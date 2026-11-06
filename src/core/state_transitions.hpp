#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
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
// UNKNOWN не считается подтверждением отсутствия необходимости восстановления.
enum class RecoveryStatus { Unknown, Required, NotRequired };
// Входные данные: подтверждения от координаторов запуска, конфигурации и диагностики.
// true устанавливается только после достоверно положительной проверки;
// отсутствие результата, UNKNOWN и ошибка должны оставаться false.
struct TransitionContext {
    bool configuration_validated{false};  // true только после успешной проверки сохранённой конфигурации.
    bool diagnostic_report_complete{false};  // true только после получения полного отчёта.
    bool critical_checks_passed{false};  // true только если все критические проверки достоверно успешны.
    bool safe_initial_condition_confirmed{false};  // true после подтверждения безопасного исходного состояния.
    // Только NOT_REQUIRED разрешает прямой переход в ОЖИДАНИЕ; значение по умолчанию UNKNOWN.
    RecoveryStatus recovery_status{RecoveryStatus::Unknown};
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


// Минимальный глобальный snapshot для фиксации переходов и чтения через F16.
// Он не содержит полной диагностики подсистем и не доказывает физическое состояние оборудования.
struct StationStateSnapshot {
    StationState state{StationState::PoweredOff};  // Последнее подтверждённое глобальное состояние.
    std::uint64_t revision{0};  // Версия: начальная предварительно 0, далее +1 на переход.
    std::string_view transition_id{};  // ID последнего подтверждённого перехода.
};

// Результат чтения последнего подтверждённого snapshot из постоянного хранилища.
enum class SnapshotReadPortStatus : std::uint8_t { Found, NotFound, Error };
struct SnapshotReadPortResult {
    SnapshotReadPortStatus status{SnapshotReadPortStatus::Error};
    std::optional<StationStateSnapshot> snapshot{};
};

// Интерфейс только для чтения. Тот же адаптер хранилища может реализовать
// его вместе с другими портами StateRepository.
class StationStateSnapshotReader {
public:
    virtual ~StationStateSnapshotReader() = default;

    // Возвращает state, revision и transition_id из одной атомарной записи.
    // Память строки transition_id должна жить, пока вызывающий использует результат.
    [[nodiscard]] virtual SnapshotReadPortResult read_snapshot() noexcept = 0;
};

enum class SnapshotReadStatus : std::uint8_t { Available, NotFound, InvalidSnapshot, ReadError };
struct SnapshotReadResult {
    SnapshotReadStatus status{SnapshotReadStatus::ReadError};
    std::optional<StationStateSnapshot> snapshot{};
};

// F16: только читает и проверяет snapshot; не записывает данные и не меняет состояние.
[[nodiscard]] SnapshotReadResult get_snapshot(
    StationStateSnapshotReader& reader) noexcept;

// Контракт чтения истории подтверждённых глобальных переходов (SSM-F17).
struct TransitionHistoryRecord {
    StationState from_state{StationState::PoweredOff};
    StationState to_state{StationState::PoweredOff};
    std::uint64_t revision{0};  // Revision после подтверждённого перехода.
    std::string event_id;       // Ссылка на исходное нормализованное событие.
    std::string transition_id;  // Уникальный ID записи перехода.
    std::optional<std::string> reason_code{};
    std::optional<std::string> reason_text{};
};

// Границы revision включительные. resulting_state фильтрует по целевому состоянию.
struct TransitionHistoryFilter {
    std::optional<std::uint64_t> minimum_revision{};
    std::optional<std::uint64_t> maximum_revision{};
    std::optional<StationState> resulting_state{};
};

enum class TransitionHistoryPortStatus : std::uint8_t {
    Complete,    // Полный набор в пределах действующей политики хранения.
    Incomplete,  // Адаптер не может подтвердить полноту выборки.
    Error        // Ошибка чтения.
};

struct TransitionHistoryPortResult {
    TransitionHistoryPortStatus status{TransitionHistoryPortStatus::Error};
    std::vector<TransitionHistoryRecord> records{};
};

class TransitionHistoryReader {
public:
    virtual ~TransitionHistoryReader() = default;

    // Читает ту же долговременную историю, в которую F10 фиксирует переходы.
    // Каждая подходящая запись возвращается ровно один раз и по возрастанию revision.
    // При COMPLETE возвращается полный соответствующий фильтру набор.
    // При INCOMPLETE частичные записи могут присутствовать, но F17 их не выдаёт.
    [[nodiscard]] virtual TransitionHistoryPortResult read_transition_history(
        const TransitionHistoryFilter& filter) noexcept = 0;
};

enum class TransitionHistoryStatus : std::uint8_t {
    Available, Empty, Incomplete, InvalidFilter, InvalidHistory, ReadError
};

struct TransitionHistoryResult {
    TransitionHistoryStatus status{TransitionHistoryStatus::ReadError};
    std::vector<TransitionHistoryRecord> records{};
};

// F17: возвращает проверенную историю без изменения snapshot, истории или оборудования.
[[nodiscard]] TransitionHistoryResult get_transition_history(
    const TransitionHistoryFilter& filter,
    TransitionHistoryReader& reader) noexcept;

// План не разрешает переход сам по себе: F10 повторно сверяет его с результатом F05.
// expected_state и expected_revision защищают от записи поверх более нового состояния.
struct TransitionPlan {
    StationState expected_state{StationState::PoweredOff};  // Состояние из текущего snapshot.
    std::uint64_t expected_revision{0};  // Версия этого snapshot; должна совпасть с сохранённой.
    StationState next_state{StationState::PoweredOff};  // Целевое состояние, ранее разрешённое F05.
    std::string_view transition_id{};  // Уникальный ID перехода; формат и генератор пока не выбраны.
};

// Доказательная часть первого вертикального среза: нормализованное событие
// и положительные подтверждения условий, используемые resolve_transition().
struct TransitionEvidence {
    Event event{};  // Событие, ранее прошедшее F01/F03/F04 у вызывающего слоя.
    TransitionContext context{};  // Подтверждения доверенных менеджеров, а не сырые данные датчиков.
};

enum class StationStateRepositoryStatus : std::uint8_t {
    Committed, DuplicateEvent, EventConflict, TransitionIdConflict, SnapshotChanged, Error
};

// Специализированный логический порт транзакции глобального состояния.
// Тот же физический адаптер хранения может реализовать его вместе с StateRepository
// из operations.hpp и OperationUpdateRepository; отдельную БД этот интерфейс не требует.
class StationStateRepository {
public:
    virtual ~StationStateRepository() = default;

    // Одна атомарная операция: сравнить expected_state/revision; проверить уникальность
    // event_id и transition_id; сохранить next_snapshot, историю переходов и исходный Event.
    // Для повторного event_id сравнивается всё содержимое, кроме назначенного приёмником received_at.
    // Ничего из перечисленного нельзя фиксировать частично. Все string_view копируются до возврата.
    [[nodiscard]] virtual StationStateRepositoryStatus commit_transition(
        const TransitionPlan& plan, const TransitionEvidence& evidence,
        const StationStateSnapshot& next_snapshot) noexcept = 0;
};

enum class StateChangedPublishStatus : std::uint8_t { Published, Rejected, Unknown };
struct StateChanged {
    StationState old_state{StationState::PoweredOff};  // Состояние до фиксации.
    StationState new_state{StationState::PoweredOff};  // Состояние после фиксации.
    std::uint64_t revision{0};  // Новая версия состояния.
    std::string_view event_id{};  // ID события, на основании которого выполнен переход.
    std::string_view transition_id{};  // ID записи перехода.
    std::optional<std::string_view> reason_code{};  // Только если причина достоверно определена.
    std::optional<std::string_view> reason_text{};  // Краткое описание известной причины.
};
class StateChangedPublisher {
public:
    virtual ~StateChangedPublisher() = default;
    // При асинхронной постановке получатель копирует/сериализует данные до возврата.
    [[nodiscard]] virtual StateChangedPublishStatus publish(
        const StateChanged& notification) noexcept = 0;
};

enum class TransitionCommitStatus : std::uint8_t {
    Committed, CommittedNotificationNotConfirmed, DuplicateEvent, EventConflict,
    TransitionIdConflict, SnapshotChanged, InvalidPlan, InvalidEvidence, RepositoryError
};
struct TransitionCommitResult {
    TransitionCommitStatus status{TransitionCommitStatus::InvalidPlan};
    StationStateSnapshot snapshot{};
    std::string_view reason_code{};
    std::string_view reason_text{};
    [[nodiscard]] bool committed() const noexcept {
        return status == TransitionCommitStatus::Committed ||
               status == TransitionCommitStatus::CommittedNotificationNotConfirmed;
    }
};

// F10: повторно проверяет допуск F05, атомарно фиксирует новый snapshot и историю,
// затем публикует StateChanged. Если публикация не подтверждена, запись уже сохранена:
// повторять commit нельзя; требуется сверка/восстановление уведомления.
[[nodiscard]] TransitionCommitResult commit_transition(
    const TransitionPlan& plan, const TransitionEvidence& evidence,
    StationStateRepository& repository, StateChangedPublisher& publisher) noexcept;

}  // namespace dockstation::core
