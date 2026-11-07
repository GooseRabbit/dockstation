#include "operation_timeout_event_processor.hpp"

namespace dockstation::core {
namespace {

// Переводит профильный результат F11 в общий статус входного контура F13.
EventProcessorStatus map_timeout_result(OperationTimeoutResultStatus status) noexcept {
    switch (status) {
        case OperationTimeoutResultStatus::Applied:
            // Lifecycle операции изменился, но глобальное состояние станции — нет.
            return EventProcessorStatus::NoStateChange;

        case OperationTimeoutResultStatus::Duplicate:
            return EventProcessorStatus::Duplicate;

        case OperationTimeoutResultStatus::EventConflict:
            return EventProcessorStatus::EventConflict;

        case OperationTimeoutResultStatus::ConcurrentOperationUpdate:
        case OperationTimeoutResultStatus::ConcurrentSourceUpdate:
            return EventProcessorStatus::ConcurrentUpdate;

        case OperationTimeoutResultStatus::InvalidEvent:
        case OperationTimeoutResultStatus::EventNotReliable:
        case OperationTimeoutResultStatus::WrongEventType:
        case OperationTimeoutResultStatus::InvalidTimeoutType:
        case OperationTimeoutResultStatus::InvalidDispatchCertainty:
        case OperationTimeoutResultStatus::OperationNotFound:
        case OperationTimeoutResultStatus::CorrelationMismatch:
        case OperationTimeoutResultStatus::TimeoutNotApplicable:
        case OperationTimeoutResultStatus::AlreadyOutcomeUnknown:
        case OperationTimeoutResultStatus::LateTimeoutIgnored:
            return EventProcessorStatus::Rejected;

        case OperationTimeoutResultStatus::StaleEvent:
        case OperationTimeoutResultStatus::EventOrderUnknown:
        case OperationTimeoutResultStatus::RepositoryError:
            return EventProcessorStatus::Error;
    }

    // Неизвестный код результата не должен трактоваться как успех.
    return EventProcessorStatus::Error;
}

}  // namespace

OperationTimeoutEventProcessor::OperationTimeoutEventProcessor(
    OperationTimeoutDecoderPort& decoder,
    OperationUpdateRepository& repository) noexcept
    : decoder_(decoder), repository_(repository) {}

EventProcessorStatus OperationTimeoutEventProcessor::process(
    const Event& event,
    EventCategory category,
    const SourceCursor& expected_cursor) noexcept {
    // Не перехватываем другие типы событий, даже если их payload похож.
    if (category != EventCategory::Timer || event.event_type != "OPERATION_TIMEOUT") {
        return EventProcessorStatus::Unsupported;
    }

    // Недостоверное событие не передаётся даже доверенному декодеру.
    if (event.quality_status != "VALID") {
        return EventProcessorStatus::Rejected;
    }

    // Формат payload остаётся ответственностью отдельного адаптера.
    const OperationTimeoutDecodeResult decoded = decoder_.decode_timeout(event);
    switch (decoded.status) {
        case OperationTimeoutDecodeStatus::Decoded:
            break;

        case OperationTimeoutDecodeStatus::UnsupportedPayload:
            return EventProcessorStatus::Rejected;

        case OperationTimeoutDecodeStatus::Error:
            return EventProcessorStatus::Error;

        default:
            // Неизвестный статус декодера закрывает обработку.
            return EventProcessorStatus::Error;
    }

    // Неизвестные значения по умолчанию не подменяются предположениями.
    if (decoded.timeout_type == OperationTimeoutType::Unknown ||
        decoded.dispatch_certainty == DispatchCertainty::Unknown) {
        return EventProcessorStatus::Rejected;
    }

    // Исходный Event и expected_cursor передаются F11 без изменения.
    const OperationTimeout timeout{
        event,
        decoded.timeout_type,
        decoded.dispatch_certainty
    };
    const OperationTimeoutResult result =
        handle_operation_timeout(timeout, expected_cursor, repository_);

    return map_timeout_result(result.status);
}

}  // namespace dockstation::core
