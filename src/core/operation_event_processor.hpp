#pragma once
#include <cstdint>
#include <optional>
#include "event_handling.hpp"
#include "operations.hpp"
namespace dockstation::core {
enum class OperationUpdateDecodeStatus : std::uint8_t { Decoded, UnsupportedPayload, Error };
struct OperationUpdateDecodeResult {
    OperationUpdateDecodeStatus status{OperationUpdateDecodeStatus::Error};
    std::optional<OperationLifecycleStatus> next_status{};
};
class OperationUpdateDecoderPort {
public:
    virtual ~OperationUpdateDecoderPort() = default;
    [[nodiscard]] virtual OperationUpdateDecodeResult decode_next_status(const Event& event) noexcept = 0;
};
class OperationUpdateEventProcessor final : public EventProcessorPort {
public:
    OperationUpdateEventProcessor(OperationUpdateDecoderPort& decoder,
                                  OperationUpdateRepository& repository) noexcept;
    [[nodiscard]] EventProcessorStatus process(const Event& event, EventCategory category,
        const SourceCursor& expected_cursor) noexcept override;
private:
    OperationUpdateDecoderPort& decoder_;
    OperationUpdateRepository& repository_;
};
}  // namespace dockstation::core
