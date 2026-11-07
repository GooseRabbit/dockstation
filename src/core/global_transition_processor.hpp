#pragma once
#include <cstdint>
#include <string_view>
#include "event_handling.hpp"
#include "state_transitions.hpp"
namespace dockstation::core {
enum class TransitionContextReadStatus : std::uint8_t { Available, Unavailable, Error };
struct TransitionContextReadResult {
    TransitionContextReadStatus status{TransitionContextReadStatus::Error};
    TransitionContext context{}; // Значим только при status == Available.
};
class TransitionContextProvider {
public:
    virtual ~TransitionContextProvider() = default;
    [[nodiscard]] virtual TransitionContextReadResult read_transition_context(
        const Event& event) noexcept = 0;
};
class TransitionIdProvider {
public:
    virtual ~TransitionIdProvider() = default;
    // При true output должен быть непустым; память действительна до завершения process().
    [[nodiscard]] virtual bool next_transition_id(std::string_view& output) noexcept = 0;
};
class GlobalTransitionProcessor final : public EventProcessorPort {
public:
    GlobalTransitionProcessor(
        StationStateSnapshotReader& snapshot_reader,
        TransitionContextProvider& context_provider,
        TransitionIdProvider& transition_id_provider,
        StationStateRepository& repository,
        StateChangedPublisher& publisher) noexcept;
    [[nodiscard]] EventProcessorStatus process(
        const Event& event, EventCategory category,
        const SourceCursor& expected_cursor) noexcept override;
private:
    StationStateSnapshotReader& snapshot_reader_;
    TransitionContextProvider& context_provider_;
    TransitionIdProvider& transition_id_provider_;
    StationStateRepository& repository_;
    StateChangedPublisher& publisher_;
};
}  // namespace dockstation::core
