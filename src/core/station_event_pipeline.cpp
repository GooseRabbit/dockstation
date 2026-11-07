#include "station_event_pipeline.hpp"

namespace dockstation::core {

StationEventPipeline::StationEventPipeline(
    EventRepository& event_repository,
    EventProcessorPort& global_processor,
    EventProcessorPort& operation_processor,
    EventProcessorPort& timeout_processor) noexcept
    : routes_{
          {"STARTUP_REQUESTED", EventCategory::Command, &global_processor},
          {"INITIALIZATION_COMPLETED", EventCategory::Subsystem, &global_processor},
          {"SELF_TEST_COMPLETED", EventCategory::Subsystem, &global_processor},
          {"OPERATION_STATUS_UPDATED", EventCategory::Operation, &operation_processor},
          {"OPERATION_TIMEOUT", EventCategory::Timer, &timeout_processor}
      },
      router_(routes_, sizeof(routes_) / sizeof(routes_[0])),
      event_repository_(event_repository) {}

bool StationEventPipeline::is_configured() const noexcept {
    return router_.is_configured();
}

EventHandlingResult StationEventPipeline::handle(
    const Event& event,
    const EventValidationPolicy& validation_policy,
    const SourceCursor& expected_cursor) noexcept {
    return handle_event(event, validation_policy, expected_cursor,
                        event_repository_, router_);
}

}  // namespace dockstation::core
