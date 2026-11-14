#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "event.hpp"

namespace dockstation::core {

// Категория зарегистрированного типа события для дальнейшей обработки SSM.
enum class EventCategory : std::uint8_t {
    Subsystem,  // Изменение состояния или диагностика отдельной подсистемы.
    Operation,  // Обновление жизненного цикла длительной операции.
    Safety,     // Изменение разрешения SafetyManager на операцию.
    Command,    // Запрос команды оператором или внутренним координатором.
    Timer,      // Срабатывание таймера или истечение времени ожидания.
    Recovery,   // Запрос восстановления или результат сверки после перезапуска.
    Unknown     // Тип события отсутствует в согласованном каталоге.
};

// Классифицирует проверенное событие по точному event_type.
// event — конверт события; используется только поле event_type.
// Функция не меняет событие и не запускает переходы или действия.
[[nodiscard]] EventCategory classify_event(const Event& event) noexcept;

// Состояние результата поиска сохранённого события по event_id.
enum class EventRepositoryLookupStatus : std::uint8_t {
    Found,     // Запись найдена; поле event содержит её снимок.
    NotFound,  // Записи с таким event_id нет.
    Error      // Хранилище не смогло достоверно выполнить поиск.
};

// Снимок найденного события. Его string_view-поля должны ссылаться на память,
// действительную до завершения вызова check_duplicate_event().
struct EventRepositoryLookupResult {
    EventRepositoryLookupStatus status{EventRepositoryLookupStatus::Error};
    Event event{};
};

// Минимальный read-only порт поиска ранее сохранённого события.
// Конкретная база данных и способ хранения этим интерфейсом не задаются.
class EventRepository {
public:
    virtual ~EventRepository() = default;

    // Ищет запись по event_id. Ошибка чтения возвращается как Error, не как NotFound.
    [[nodiscard]] virtual EventRepositoryLookupResult find_by_id(
        std::string_view event_id) noexcept = 0;
};

// Результат проверки повторной доставки.
enum class DuplicateEventStatus : std::uint8_t {
    New,             // event_id не найден в репозитории.
    Duplicate,       // event_id найден, неизменяемое содержимое совпадает.
    Conflict,        // event_id найден, но содержимое отличается.
    RepositoryError  // Ошибка или противоречивый ответ репозитория.
};

struct DuplicateEventResult {
    DuplicateEventStatus status{DuplicateEventStatus::RepositoryError};
};

// Проверяет ранее сохранённую запись. Event должен быть предварительно проверен
// через validate_event(). Функция не меняет событие и не записывает его в хранилище.
[[nodiscard]] DuplicateEventResult check_duplicate_event(
    const Event& event,
    EventRepository& repository) noexcept;


// Результат сравнения номера события с последним принятым номером источника.
enum class EventFreshnessStatus : std::uint8_t {
    Current,    // Номер новее либо это первое событие подтверждённой сессии.
    Stale,      // Номер уменьшился в пределах той же сессии источника.
    Unordered   // По имеющимся данным нельзя безопасно установить порядок.
};

// Причина поясняет результат; неизвестная причина не подменяется предположением.
enum class EventFreshnessReason : std::uint8_t {
    FirstSequence,
    SequenceAdvanced,
    SequenceRepeated,
    SequenceRegressed,
    SequenceMissing,
    SourceMismatch,
    SourceInstanceMismatch
};

// Курсор одного конкретного источника событий и его текущей сессии.
// source обязателен даже до появления первого номера; пустой/чужой источник
// не позволяет установить порядок. string_view не владеет памятью: исходные
// строки должны оставаться действительными до завершения обработки события
// и профильного commit, использующего этот курсор.
// Для подтверждённой новой сессии вызывающий слой сбрасывает last_sequence и
// заранее записывает новый source_instance, затем передаёт первое событие сессии.
struct SourceCursor {
    std::string_view source{};  // Идентификатор источника; должен совпадать с Event::source.
    std::optional<std::string_view> source_instance{};  // Идентификатор запуска/сессии, если источник его передаёт.
    std::optional<std::uint64_t> last_sequence{};  // Последний принятый Event::source_sequence.
};

// Статус read-only поиска сохранённого курсора источника.
enum class SourceCursorReadStatus : std::uint8_t {
    Found,    // Для source есть сохранённый курсор.
    NotFound, // Для source нет сохранённой записи.
    Error     // Чтение не удалось или результат нельзя подтвердить.
};

// Один согласованный снимок source/source_instance/last_sequence.
// При NotFound и Error поле cursor должно оставаться пустым.
struct SourceCursorReadResult {
    SourceCursorReadStatus status{SourceCursorReadStatus::Error};
    SourceCursor cursor{};
};

// Read-only порт загрузки курсора по точному непустому source.
// Не создаёт, не сбрасывает и не продвигает курсор. Реализация обязана хранить
// строки string_view до завершения обработки события и профильного commit;
// параллельное чтение не должно инвалидировать уже возвращённый снимок.
class SourceCursorReader {
public:
    virtual ~SourceCursorReader() = default;

    [[nodiscard]] virtual SourceCursorReadResult read_source_cursor(
        std::string_view source) noexcept = 0;
};

// Проверяет согласованность формы ответа до использования cursor вызывающим слоем.
// Error является допустимым результатом чтения только с пустым cursor и всё равно
// блокирует обработку события. Неизвестный статус или противоречивый payload отклоняется.
[[nodiscard]] bool is_valid_source_cursor_read_result(
    std::string_view requested_source,
    const SourceCursorReadResult& result) noexcept;

struct EventFreshnessResult {
    EventFreshnessStatus status{EventFreshnessStatus::Unordered};
    EventFreshnessReason reason{EventFreshnessReason::SequenceMissing};
};

// Чистое сравнение. Конверт события должен быть заранее проверен через validate_event().
// Функция не использует календарные метки времени, не меняет курсор и не хранит события.
[[nodiscard]] EventFreshnessResult check_event_freshness(
    const Event& event,
    const SourceCursor& cursor) noexcept;

}  // namespace dockstation::core
