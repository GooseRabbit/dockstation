#pragma once
#include <string_view>
namespace dockstation::core {
enum class FailureDispositionStatus { KnownPreliminary, UnknownFailure };
struct Failure { std::string_view failure_id{}; };
struct FailureDispositionResult {
 FailureDispositionStatus status{FailureDispositionStatus::UnknownFailure};
 std::string_view failure_id{};
 std::string_view preliminary_reaction{};
 [[nodiscard]] explicit operator bool() const noexcept { return status == FailureDispositionStatus::KnownPreliminary; }
};
[[nodiscard]] FailureDispositionResult resolve_failure_disposition(const Failure& failure) noexcept;
} // namespace dockstation::core
