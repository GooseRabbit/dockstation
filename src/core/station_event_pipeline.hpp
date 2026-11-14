#pragma once
#include <optional>
#include "event_handling.hpp"
#include "event_processor_router.hpp"
#include "source_session_coordinator.hpp"

namespace dockstation::core {

struct StationEventPipelineResult {
    // Empty when F01/F02 rejects the event before session state can be changed.
    std::optional<SourceSessionCoordinatorStatus> session_status{};
    std::optional<EventHandlingResult> event_result{};
};

class StationEventPipeline final {
public:
    StationEventPipeline(
        EventRepository& event_repository,
        EventProcessorPort& global_processor,
        EventProcessorPort& operation_processor,
        EventProcessorPort& timeout_processor,
        SourceSessionVerifier& session_verifier,
        SourceCursorReader& cursor_reader,
        SourceSessionRepository& session_repository) noexcept;

    StationEventPipeline(const StationEventPipeline&) = delete;
    StationEventPipeline& operator=(const StationEventPipeline&) = delete;
    StationEventPipeline(StationEventPipeline&&) = delete;
    StationEventPipeline& operator=(StationEventPipeline&&) = delete;

    [[nodiscard]] bool is_configured() const noexcept;

    // Evidence поступает только из доверенного контекста аутентифицированного
    // транспорта, не из Event, пользовательского API или payload.
    // Произвольный SourceCursor вызывающий код передать не может.
    [[nodiscard]] StationEventPipelineResult handle(
        const Event& event,
        const EventValidationPolicy& validation_policy,
        const SourceSessionEvidence& evidence) noexcept;

private:
    const EventProcessorRoute routes_[5];
    EventProcessorRouter router_;
    EventRepository& event_repository_;
    SourceSessionCoordinator session_coordinator_;
};

}  // namespace dockstation::core
