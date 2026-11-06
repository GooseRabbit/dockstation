#include "event_handling.hpp"

namespace dockstation::core {

EventHandlingResult handle_event(
    const Event& event,                              // Нормализованный конверт без разбора payload.
    const EventValidationPolicy& validation_policy,  // Политика обязательных полей от доверенного адаптера источника.
    const SourceCursor& source_cursor,                // Последний принятый номер этого источника/сессии.
    EventRepository& event_repository,                // Read-only поиск ранее принятого event_id.
    EventProcessorPort& processor) noexcept {         // Профильный обработчик категории события.

    // F01: сначала проверяем конверт. Невалидное событие не должно обращаться к хранилищу или обработчику.
    const EventValidationResult validation = validate_event(event, validation_policy);
    if (!validation.valid) {
        return {EventHandlingStatus::InvalidEvent, EventCategory::Unknown,
                validation.error, EventFreshnessReason::SequenceMissing};
    }

    // F02: неизвестный тип не маршрутизируем по догадке.
    const EventCategory category = classify_event(event);
    if (category == EventCategory::Unknown) {
        return {EventHandlingStatus::UnknownEventType, category,
                EventValidationError::None, EventFreshnessReason::SequenceMissing};
    }

    // F03: конфликт или ошибка чтения закрывают обработку; повтор не выполняется второй раз.
    const DuplicateEventResult duplicate = check_duplicate_event(event, event_repository);
    switch (duplicate.status) {
        case DuplicateEventStatus::Duplicate:
            return {EventHandlingStatus::Duplicate, category,
                    EventValidationError::None, EventFreshnessReason::SequenceMissing};
        case DuplicateEventStatus::Conflict:
            return {EventHandlingStatus::EventConflict, category,
                    EventValidationError::None, EventFreshnessReason::SequenceMissing};
        case DuplicateEventStatus::RepositoryError:
            return {EventHandlingStatus::RepositoryError, category,
                    EventValidationError::None, EventFreshnessReason::SequenceMissing};
        case DuplicateEventStatus::New:
            break;
    }

    // F04: допускаем только доказуемо текущую последовательность источника/сессии.
    const EventFreshnessResult freshness = check_event_freshness(event, source_cursor);
    if (freshness.status == EventFreshnessStatus::Stale) {
        return {EventHandlingStatus::StaleEvent, category,
                EventValidationError::None, freshness.reason};
    }
    if (freshness.status != EventFreshnessStatus::Current) {
        return {EventHandlingStatus::EventOrderUnknown, category,
                EventValidationError::None, freshness.reason};
    }

    // Передаём событие профильному обработчику только после прохождения всех общих ворот.
    // Сам F13 не меняет курсор: это входит в транзакцию профильного обработчика.
    const EventProcessorStatus processed = processor.process(event, category, source_cursor);
    switch (processed) {
        case EventProcessorStatus::Processed:
            return {EventHandlingStatus::Processed, category,
                    EventValidationError::None, freshness.reason};
        case EventProcessorStatus::NoStateChange:
            return {EventHandlingStatus::NoStateChange, category,
                    EventValidationError::None, freshness.reason};
        case EventProcessorStatus::Rejected:
            return {EventHandlingStatus::Rejected, category,
                    EventValidationError::None, freshness.reason};
        case EventProcessorStatus::Unsupported:
            return {EventHandlingStatus::Unsupported, category,
                    EventValidationError::None, freshness.reason};
        case EventProcessorStatus::Duplicate:
            return {EventHandlingStatus::Duplicate, category,
                    EventValidationError::None, freshness.reason};
        case EventProcessorStatus::EventConflict:
            return {EventHandlingStatus::EventConflict, category,
                    EventValidationError::None, freshness.reason};
        case EventProcessorStatus::ConcurrentUpdate:
            return {EventHandlingStatus::ConcurrentUpdate, category,
                    EventValidationError::None, freshness.reason};
        case EventProcessorStatus::Error:
            return {EventHandlingStatus::ProcessingError, category,
                    EventValidationError::None, freshness.reason};
    }

    // Неизвестное значение enum из внешнего адаптера не трактуем как успех.
    return {EventHandlingStatus::ProcessingError, category,
            EventValidationError::None, freshness.reason};
}

}  // namespace dockstation::core
