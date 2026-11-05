#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "event_processing.hpp"

namespace dockstation::core {

// Логический план одного действия. Он не содержит команд GPIO или параметров драйвера.
// string_view не владеет памятью: строки должны оставаться действительными до сохранения запроса.
struct ActionPlan {
    std::string_view target_module;  // Логический получатель запроса, например идентификатор функционального модуля.
    std::string_view action;         // Логическое имя действия; каталог действий пока не утверждён.
    std::optional<std::string_view> parameters;  // Непрозрачные параметры; схема зависит от будущего контракта модуля.
    std::string_view correlation_id; // Связь с исходным событием/командой; формируется вызывающим слоем.
};

// Идентификатор выдаётся внешним поставщиком; формат и алгоритм здесь не закрепляются.
class OperationIdProvider {
public:
    virtual ~OperationIdProvider() = default;

    // При успехе записывает непустой ID в output. Выданное значение должно оставаться
    // действительным и уникальным относительно ранее выданных/сохранённых операций.
    [[nodiscard]] virtual bool next_operation_id(std::string_view& output) noexcept = 0;
};

struct ActionRequest {
    std::string_view operation_id;
    std::string_view correlation_id;
    std::string_view target_module;
    std::string_view action;
    std::optional<std::string_view> parameters;
};

enum class ActionRequestStatus {
    Created,
    InvalidPlan,
    OperationIdUnavailable,
    InvalidOperationId
};

struct ActionRequestResult {
    ActionRequestStatus status{ActionRequestStatus::InvalidPlan};
    ActionRequest request{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == ActionRequestStatus::Created;
    }
};

// Формирует логический запрос и назначает ему ID. Функция не сохраняет запрос,
// не отправляет его исполнителю и не управляет физическим оборудованием.
// Параметры: plan — логический план; id_provider — порт выдачи уникального operation_id.
[[nodiscard]] ActionRequestResult create_action_request(
    const ActionPlan& plan,
    OperationIdProvider& id_provider) noexcept;


// Результат атомарной попытки сохранить намерение по operation_id.
enum class OperationIntentSaveStatus {
    Saved,             // Новая запись устойчиво сохранена; lifecycle установлен в REQUESTED.
    AlreadySavedSame,  // Такой operation_id уже сохранён с полностью совпадающим запросом.
    Conflict,          // operation_id уже существует, но хотя бы одно поле запроса отличается.
    Error              // Хранилище не смогло подтвердить результат сохранения.
};

// Логический порт долговременного хранения состояния SSM.
// Реальная БД, файл, NVS или иной накопитель этим интерфейсом не выбираются.
class StateRepository {
public:
    virtual ~StateRepository() = default;

    // Saved допустим только после атомарной фиксации записи, переживающей перезапуск.
    // Новая запись создаёт OperationRecord со статусом Requested и revision=1.
    // При повторном ID репозиторий сравнивает весь ActionRequest точно, включая optional parameters.
    // До возврата данные из string_view должны быть скопированы/сериализованы во владеющее хранилище.
    [[nodiscard]] virtual OperationIntentSaveStatus save_operation_intent(
        const ActionRequest& request) noexcept = 0;
};

enum class PersistStatus {
    Persisted,         // Новое намерение устойчиво сохранено; допускается последующая передача в F08.
    AlreadyPersisted,  // Точное намерение уже есть; повторно отправлять запрос нельзя.
    Conflict,          // Тот же operation_id связан с другими данными.
    RepositoryError,   // Ошибка или неизвестный ответ StateRepository.
    InvalidRequest     // В запросе отсутствует обязательное поле.
};

struct PersistResult {
    PersistStatus status{PersistStatus::InvalidRequest};
    ActionRequest persisted_request{};

    [[nodiscard]] bool authorizes(const ActionRequest& request) const noexcept;
};

// Сохраняет намерение до передачи физически значимого действия исполнителю.
// Параметры: request — сформированный ActionRequest; repository — логический порт StateRepository.
// Функция не запускает диспетчер и не выполняет физических действий.
[[nodiscard]] PersistResult persist_operation_intent(
    const ActionRequest& request,
    StateRepository& repository) noexcept;

enum class ActionDispatchPortStatus { Accepted, AlreadyKnown, Rejected, Conflict, Unknown };

class ActionDispatcher {
public:
    virtual ~ActionDispatcher() = default;
    // Получатель дедуплицирует operation_id; точный повтор не запускает действие заново.
    // Тот же ID с иным содержимым — Conflict; неопределённый результат — Unknown.
    // Данные string_view должны быть скопированы/сериализованы до возврата при асинхронной постановке в очередь.
    [[nodiscard]] virtual ActionDispatchPortStatus dispatch(const ActionRequest& request) noexcept = 0;
};

enum class DispatchStatus {
    Accepted, AlreadyKnown, Rejected, Conflict, OutcomeUnknown,
    PersistenceNotConfirmed, PersistenceReceiptMismatch, InvalidRequest
};

struct DispatchResult {
    DispatchStatus status{DispatchStatus::InvalidRequest};
};

[[nodiscard]] DispatchResult dispatch_action_request(
    const ActionRequest& request,
    const PersistResult& persistence,
    ActionDispatcher& dispatcher) noexcept;


/** Жизненный цикл операции отделён от глобального состояния станции. */
enum class OperationLifecycleStatus : std::uint8_t {
    Requested, Accepted, Started, Completed, Failed, CancelRequested, Cancelled, OutcomeUnknown
};

// Сохранённая операция. F07 создаёт её со статусом Requested и revision=1.
struct OperationRecord {
    ActionRequest request{};
    OperationLifecycleStatus status{OperationLifecycleStatus::Requested};
    std::uint64_t revision{1};
};

enum class OperationLookupStatus : std::uint8_t { Found, NotFound, Error };
struct OperationLookupResult {
    OperationLookupStatus status{OperationLookupStatus::Error};
    OperationRecord record{};
};

enum class OperationUpdateCommitStatus : std::uint8_t {
    Applied, DuplicateEvent, EventConflict, OperationChanged,
    SourceCursorChanged, OperationNotFound, Error
};

// Единый логический порт для F09. Операции, события и курсор должны иметь
// согласованное долговременное представление.
class OperationUpdateRepository : public EventRepository {
public:
    ~OperationUpdateRepository() override = default;

    // Снимок operation_id; string_view должны оставаться действительными до завершения commit_operation_update().
    [[nodiscard]] virtual OperationLookupResult find_operation(
        std::string_view operation_id) noexcept = 0;

    // Одна атомарная фиксация: проверка revision/status и курсора; изменение
    // lifecycle; сохранение event для find_by_id(); продвижение source_sequence.
    // При конфликте операции/курсора возвращается соответствующий статус.
    // Повторный event_id возвращает DuplicateEvent, иное содержимое — EventConflict.
    // Все string_view копируются/сериализуются до возврата.
    [[nodiscard]] virtual OperationUpdateCommitStatus commit_operation_update(
        const OperationRecord& expected_record,
        OperationLifecycleStatus next_status,
        const Event& event,
        const SourceCursor& expected_cursor) noexcept = 0;
};

// Адаптер уже преобразовал payload события в типизированный статус.
// F09 не выбирает JSON и не угадывает значение по строке.
struct OperationUpdate {
    Event event{};
    OperationLifecycleStatus next_status{OperationLifecycleStatus::Requested};
};

enum class OperationUpdateResultStatus : std::uint8_t {
    Applied, Duplicate, EventConflict, InvalidEvent, EventNotReliable,
    WrongEventType, InvalidStatus, StaleEvent, EventOrderUnknown,
    OperationNotFound, CorrelationMismatch, TransitionNotAllowed,
    ConcurrentOperationUpdate, ConcurrentSourceUpdate, RepositoryError
};

struct OperationUpdateResult {
    OperationUpdateResultStatus status{OperationUpdateResultStatus::InvalidEvent};
    OperationLifecycleStatus resulting_status{OperationLifecycleStatus::Requested};
    std::uint64_t revision{0};
};

// update — событие и распознанный статус; source_cursor — последний принятый
// номер источника/сессии; repository — атомарный порт операций и событий.
// Функция не меняет глобальное состояние станции и не запускает оборудование.
[[nodiscard]] OperationUpdateResult handle_operation_update(
    const OperationUpdate& update,
    const SourceCursor& source_cursor,
    OperationUpdateRepository& repository) noexcept;

}  // namespace dockstation::core
