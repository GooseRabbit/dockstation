#include "station_event_pipeline.hpp"

namespace dockstation::core {
namespace {

class UnsupportedSubsystemStatusProcessor final : public EventProcessorPort {
public:
    [[nodiscard]] EventProcessorStatus process(
        const Event&, EventCategory,
        const SourceCursor&) noexcept override {
        return EventProcessorStatus::Unsupported;
    }
};

EventProcessorPort& unavailable_subsystem_status_processor() noexcept {
    static UnsupportedSubsystemStatusProcessor processor;
    return processor;
}

}  // namespace

StationEventPipeline::StationEventPipeline(
    EventRepository& event_repository,
    EventProcessorPort& global_processor,
    EventProcessorPort& operation_processor,
    EventProcessorPort& timeout_processor,
    SourceSessionVerifier& session_verifier,
    SourceCursorReader& cursor_reader,
    SourceSessionRepository& session_repository,
    EventProcessorPort* subsystem_status_processor) noexcept
    : routes_{
          {"STARTUP_REQUESTED", EventCategory::Command, &global_processor},
          {"INITIALIZATION_COMPLETED", EventCategory::Subsystem, &global_processor},
          {"SELF_TEST_COMPLETED", EventCategory::Subsystem, &global_processor},
          {"OPERATION_STATUS_UPDATED", EventCategory::Operation, &operation_processor},
          {"OPERATION_TIMEOUT", EventCategory::Timer, &timeout_processor},
          {"SUBSYSTEM_STATUS_UPDATED", EventCategory::Subsystem,
           subsystem_status_processor != nullptr
               ? subsystem_status_processor
               : &unavailable_subsystem_status_processor()}
      },
      router_(routes_, sizeof(routes_) / sizeof(routes_[0])),
      event_repository_(event_repository),
      session_coordinator_(session_verifier, cursor_reader, session_repository) {}

bool StationEventPipeline::is_configured() const noexcept {
    return router_.is_configured();
}

StationEventPipelineResult StationEventPipeline::handle(
    const Event& event,
    const EventValidationPolicy& validation_policy,
    const SourceSessionEvidence& evidence) noexcept {
    // F01/F02 are pure preflight checks. Reject malformed/unknown events before
    // session confirmation can create or replace a persistent source cursor.
    const EventValidationResult validation =
        validate_event(event, validation_policy);
    if (!validation.valid) {
        return {std::nullopt,
                EventHandlingResult{EventHandlingStatus::InvalidEvent,
                                    EventCategory::Unknown,
                                    validation.error,
                                    EventFreshnessReason::SequenceMissing}};
    }

    const EventCategory category = classify_event(event);
    if (category == EventCategory::Unknown) {
        return {std::nullopt,
                EventHandlingResult{EventHandlingStatus::UnknownEventType,
                                    category,
                                    EventValidationError::None,
                                    EventFreshnessReason::SequenceMissing}};
    }

    // handle_event() repeats F01/F02 defensively before F03/F04 and dispatch.
    const SourceSessionCoordinatorResult session =
        session_coordinator_.confirm(event, evidence);
    if (session.status != SourceSessionCoordinatorStatus::Confirmed &&
        session.status != SourceSessionCoordinatorStatus::AlreadyCurrent) {
        return {session.status, std::nullopt};
    }

    const EventHandlingResult handled =
        handle_event(event, validation_policy, session.cursor,
                     event_repository_, router_);
    return {session.status, handled};
}

}  // namespace dockstation::core
