#pragma once
#include <cstdint>
#include "event_handling.hpp"
#include "operations.hpp"
namespace dockstation::core {
enum class OperationTimeoutDecodeStatus : std::uint8_t { Decoded, UnsupportedPayload, Error };
struct OperationTimeoutDecodeResult {
    OperationTimeoutDecodeStatus status{OperationTimeoutDecodeStatus::Error};
    OperationTimeoutType timeout_type{OperationTimeoutType::Unknown};
    DispatchCertainty dispatch_certainty{DispatchCertainty::Unknown};
};
class OperationTimeoutDecoderPort {
public:
    virtual ~OperationTimeoutDecoderPort() = default;
    [[nodiscard]] virtual OperationTimeoutDecodeResult decode_timeout(const Event& event) noexcept = 0;
};
class OperationTimeoutEventProcessor final : public EventProcessorPort {
public:
    OperationTimeoutEventProcessor(OperationTimeoutDecoderPort& decoder,
                                   OperationUpdateRepository& repository) noexcept;
    [[nodiscard]] EventProcessorStatus process(const Event& event, EventCategory category,
        const SourceCursor& expected_cursor) noexcept override;
private:
    OperationTimeoutDecoderPort& decoder_;
    OperationUpdateRepository& repository_;
};
}  // namespace dockstation::core
