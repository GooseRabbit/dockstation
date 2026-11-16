#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "communications/authenticated_event_transport_adapter.hpp"
#include "hardware/interfaces/controller_codec.hpp"

namespace dockstation::communications {

// Projects only ERROR messages that explicitly identify both a subsystem and
// its reported state. TELEMETRY_SNAPSHOT and generic ERROR reports remain in
// the separate diagnostics/report stream and never enter the SSM event pipe.
class ControllerSubsystemStatusEventProjector final
    : public ControllerEventProjector {
public:
    static constexpr std::size_t kPayloadCapacity =
        hardware::interfaces::kControllerCodecMaximumFrameSize * 6U + 256U;

    [[nodiscard]] ControllerEventProjectionStatus project(
        const hardware::interfaces::ControllerCodecDecodedMessage& message,
        ControllerEventProjection& output) noexcept override;

private:
    [[nodiscard]] bool append(std::string_view value) noexcept;
    [[nodiscard]] bool append_json_string(std::string_view value) noexcept;
    void reset() noexcept;

    std::array<char, kPayloadCapacity> payload_{};
    std::size_t payload_size_{0U};
};

}  // namespace dockstation::communications
