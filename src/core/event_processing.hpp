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
// строки должны жить до завершения вызова.
// Для подтверждённой новой сессии вызывающий слой сбрасывает last_sequence и
// заранее записывает новый source_instance, затем передаёт первое событие сессии.
struct SourceCursor {
    std::string_view source{};  // Идентификатор источника; должен совпадать с Event::source.
    std::optional<std::string_view> source_instance{};  // Идентификатор запуска/сессии, если источник его передаёт.
    std::optional<std::uint64_t> last_sequence{};  // Последний принятый Event::source_sequence.
};

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
