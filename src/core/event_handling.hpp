#pragma once

#include <cstdint>

#include "event_processing.hpp"
#include "event_validation.hpp"

namespace dockstation::core {

enum class EventProcessorStatus : std::uint8_t {
    Processed,         // Профильная обработка завершена и результат подтверждён.
    NoStateChange,     // Событие принято; глобальное состояние станции не изменилось.
    Rejected,          // Профильные правила отклонили событие.
    Unsupported,       // Для типа/категории пока нет профильного обработчика.
    Duplicate,         // Повтор обнаружен при атомарной повторной проверке.
    EventConflict,     // event_id уже связан с другим содержимым.
    ConcurrentUpdate,  // Ожидаемое состояние/курсор изменились до фиксации.
    Error              // Результат обработки или фиксации не подтверждён.
};

class EventProcessorPort {
public:
    virtual ~EventProcessorPort() = default;

    // До любого побочного эффекта реализация повторно проверяет event_id и курсор
    // атомарно с фиксацией результата. Для Processed/NoStateChange она подтверждает
    // запись события и курсора вместе с профильным изменением, если оно требуется.
    // Для физической команды используются F07/F08 и актуальный ALLOW исполнителя.
    // Формат payload и обработчики категорий остаются вне общего контура F13.
    [[nodiscard]] virtual EventProcessorStatus process(
        const Event& event,
        EventCategory category,
        const SourceCursor& expected_cursor) noexcept = 0;
};

enum class EventHandlingStatus : std::uint8_t {
    Processed, NoStateChange, Rejected, Unsupported,
    Duplicate, EventConflict, InvalidEvent, UnknownEventType,
    StaleEvent, EventOrderUnknown, RepositoryError,
    ConcurrentUpdate, ProcessingError
};

struct EventHandlingResult {
    EventHandlingStatus status{EventHandlingStatus::InvalidEvent};
    EventCategory category{EventCategory::Unknown};
    EventValidationError validation_error{EventValidationError::None};
    EventFreshnessReason freshness_reason{EventFreshnessReason::SequenceMissing};
};

// F13: проверяет структуру конверта, каталог типа, повтор и порядок события, затем
// передаёт его профильному обработчику. Не разбирает payload и не управляет
// глобальным состоянием или физическим оборудованием самостоятельно.
[[nodiscard]] EventHandlingResult handle_event(
    const Event& event,
    const EventValidationPolicy& validation_policy,
    const SourceCursor& source_cursor,
    EventRepository& event_repository,
    EventProcessorPort& processor) noexcept;

}  // namespace dockstation::core
