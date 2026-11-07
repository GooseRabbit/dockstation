#include "operation_event_processor.hpp"
namespace dockstation::core {
namespace {
EventProcessorStatus map_operation_update_result(OperationUpdateResultStatus status) noexcept {
    switch (status) {
        case OperationUpdateResultStatus::Applied:
            return EventProcessorStatus::NoStateChange;
        case OperationUpdateResultStatus::Duplicate:
            return EventProcessorStatus::Duplicate;
        case OperationUpdateResultStatus::EventConflict:
            return EventProcessorStatus::EventConflict;
        case OperationUpdateResultStatus::InvalidEvent:
        case OperationUpdateResultStatus::EventNotReliable:
        case OperationUpdateResultStatus::WrongEventType:
        case OperationUpdateResultStatus::InvalidStatus:
        case OperationUpdateResultStatus::OperationNotFound:
        case OperationUpdateResultStatus::CorrelationMismatch:
        case OperationUpdateResultStatus::TransitionNotAllowed:
            return EventProcessorStatus::Rejected;
        case OperationUpdateResultStatus::ConcurrentOperationUpdate:
        case OperationUpdateResultStatus::ConcurrentSourceUpdate:
            return EventProcessorStatus::ConcurrentUpdate;
        case OperationUpdateResultStatus::RepositoryError:
        case OperationUpdateResultStatus::StaleEvent:
        case OperationUpdateResultStatus::EventOrderUnknown:
            return EventProcessorStatus::Error;
    }
    return EventProcessorStatus::Error;
}
}  // namespace
OperationUpdateEventProcessor::OperationUpdateEventProcessor(
    OperationUpdateDecoderPort& decoder, OperationUpdateRepository& repository) noexcept
    : decoder_(decoder), repository_(repository) {}
EventProcessorStatus OperationUpdateEventProcessor::process(
    const Event& event, EventCategory category, const SourceCursor& expected_cursor) noexcept {
    if (category != EventCategory::Operation || event.event_type != "OPERATION_STATUS_UPDATED")
        return EventProcessorStatus::Unsupported;
    if (event.quality_status != "VALID") return EventProcessorStatus::Rejected;
    const OperationUpdateDecodeResult decoded = decoder_.decode_next_status(event);
    switch (decoded.status) {
        case OperationUpdateDecodeStatus::Decoded: break;
        case OperationUpdateDecodeStatus::UnsupportedPayload: return EventProcessorStatus::Rejected;
        case OperationUpdateDecodeStatus::Error: return EventProcessorStatus::Error;
        default: return EventProcessorStatus::Error;
    }
    if (!decoded.next_status.has_value()) return EventProcessorStatus::Error;
    const OperationUpdate update{event, *decoded.next_status};
    const OperationUpdateResult result = handle_operation_update(update, expected_cursor, repository_);
    return map_operation_update_result(result.status);
}
}  // namespace dockstation::core
