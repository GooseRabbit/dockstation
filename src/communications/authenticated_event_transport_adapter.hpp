#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "core/station_event_pipeline.hpp"
#include "platform/linux/controller_link_service.hpp"

namespace dockstation::communications {

// The projector maps one already-authenticated controller application message
// to the domain-specific event fields. It must not supply source, instance,
// sequence, event_id, or received_at: those are bound by the transport adapter.
// All returned views must remain valid until project() and the synchronous
// StationEventPipeline::handle() call have both completed.
enum class ControllerEventProjectionStatus : std::uint8_t {
    Projected,
    NotApplicable,
    Error
};

struct ControllerEventProjection final {
    std::string_view event_type{};
    std::optional<std::string_view> occurred_at{};
    std::optional<std::string_view> operation_id{};
    std::optional<std::string_view> correlation_id{};
    std::string_view payload{};
    std::string_view quality_status{};
    std::string_view schema_version{};
    std::optional<std::string_view> reason_code{};
    std::optional<std::string_view> reason_text{};
};

class ControllerEventProjector {
public:
    virtual ~ControllerEventProjector() = default;

    [[nodiscard]] virtual ControllerEventProjectionStatus project(
        const hardware::interfaces::ControllerCodecDecodedMessage& message,
        ControllerEventProjection& output) noexcept = 0;
};

enum class AuthenticatedEventTransportStatus : std::uint8_t {
    InvalidConfiguration,
    Ready,
    MessageIgnored,
    ChannelUnavailable,
    MessageBindingInvalid,
    ProjectionNotApplicable,
    ProjectionError,
    EventIdentifierUnavailable,
    ReceiveTimestampUnavailable,
    SessionRejected,
    PipelineInvoked
};

struct AuthenticatedEventTransportStatistics final {
    AuthenticatedEventTransportStatus last_status{
        AuthenticatedEventTransportStatus::InvalidConfiguration
    };
    std::uint64_t authenticated_messages_received{0U};
    std::uint64_t projected_events{0U};
    std::uint64_t pipeline_invocations{0U};
    std::uint64_t rejected_messages{0U};
    std::uint64_t unmapped_messages{0U};
    std::optional<core::SourceSessionCoordinatorStatus> last_session_status{};
    std::optional<core::EventHandlingStatus> last_event_status{};
};

// This adapter is attached to the ControllerLinkService before it starts.
// The service invokes it synchronously only after TLS authentication and
// ControllerLink envelope/sequence validation have accepted a telemetry or
// error message. It builds Event and opaque SourceSessionEvidence from that
// same decoded frame and live channel, then calls StationEventPipeline before
// the frame buffer or exporter output can be invalidated.
class AuthenticatedEventTransportAdapter final
    : public platform::linux_platform::ControllerLinkMessageHandler {
public:
    static constexpr std::size_t kMaximumSourceBytes = 128U;
    static constexpr std::size_t kEventIdCapacity = 384U;

    AuthenticatedEventTransportAdapter(
        platform::linux_platform::ControllerLinkService& service,
        core::StationEventPipeline& pipeline,
        ControllerEventProjector& projector,
        std::string_view configured_source,
        core::EventValidationPolicy validation_policy = {}) noexcept;
    ~AuthenticatedEventTransportAdapter() override;

    AuthenticatedEventTransportAdapter(
        const AuthenticatedEventTransportAdapter&) = delete;
    AuthenticatedEventTransportAdapter& operator=(
        const AuthenticatedEventTransportAdapter&) = delete;
    AuthenticatedEventTransportAdapter(
        AuthenticatedEventTransportAdapter&&) = delete;
    AuthenticatedEventTransportAdapter& operator=(
        AuthenticatedEventTransportAdapter&&) = delete;

    [[nodiscard]] bool is_configured() const noexcept { return attached_; }
    [[nodiscard]] const AuthenticatedEventTransportStatistics&
    statistics() const noexcept { return statistics_; }

    void on_authenticated_controller_message(
        platform::linux_platform::ControllerLinkService& service,
        const hardware::interfaces::ControllerCodecDecodedMessage& message,
        std::uint64_t received_at_unix_ms) noexcept override;

private:
    [[nodiscard]] bool copy_source(std::string_view source) noexcept;

    platform::linux_platform::ControllerLinkService& service_;
    core::StationEventPipeline& pipeline_;
    ControllerEventProjector& projector_;
    std::array<char, kMaximumSourceBytes> source_{};
    std::size_t source_size_{0U};
    core::EventValidationPolicy validation_policy_{};
    AuthenticatedEventTransportStatistics statistics_{};
    bool attached_{false};
};

}  // namespace dockstation::communications
