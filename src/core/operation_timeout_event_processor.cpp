#include "operation_timeout_event_processor.hpp"
namespace dockstation::core {
namespace {
EventProcessorStatus map_timeout_result(OperationTimeoutResultStatus status) noexcept {
    switch(status) {
        case OperationTimeoutResultStatus::Applied:
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
    return EventProcessorStatus::Error;
}
}
OperationTimeoutEventProcessor::OperationTimeoutEventProcessor(
    OperationTimeoutDecoderPort& decoder, OperationUpdateRepository& repository) noexcept
    : decoder_(decoder), repository_(repository) {}
EventProcessorStatus OperationTimeoutEventProcessor::process(
    const Event& event, EventCategory category, const SourceCursor& expected_cursor) noexcept {
    if(category!=EventCategory::Timer || event.event_type!="OPERATION_TIMEOUT")
        return EventProcessorStatus::Unsupported;
    if(event.quality_status!="VALID") return EventProcessorStatus::Rejected;
    const auto decoded=decoder_.decode_timeout(event);
    switch(decoded.status) {
        case OperationTimeoutDecodeStatus::Decoded: break;
        case OperationTimeoutDecodeStatus::UnsupportedPayload: return EventProcessorStatus::Rejected;
        case OperationTimeoutDecodeStatus::Error: return EventProcessorStatus::Error;
        default: return EventProcessorStatus::Error;
    }
    if(decoded.timeout_type==OperationTimeoutType::Unknown ||
       decoded.dispatch_certainty==DispatchCertainty::Unknown)
        return EventProcessorStatus::Rejected;
    const OperationTimeout timeout{event,decoded.timeout_type,decoded.dispatch_certainty};
    const auto result=handle_operation_timeout(timeout,expected_cursor,repository_);
    return map_timeout_result(result.status);
}
}  // namespace dockstation::core
