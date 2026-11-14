#pragma once
#include <string_view>
#include <type_traits>
#include "core/source_session.hpp"
namespace dockstation::verification {
class SourceSessionEvidenceTestAccess final {
public:
    [[nodiscard]] static core::SourceSessionEvidence make(
        std::string_view source, std::string_view source_instance,
        std::string_view peer_identity_evidence, std::string_view session_proof) noexcept {
        return core::SourceSessionEvidence{source, source_instance,
                                           peer_identity_evidence, session_proof};
    }
};
static_assert(!std::is_aggregate<core::SourceSessionEvidence>::value,
              "SourceSessionEvidence must not be aggregate-constructible");
static_assert(!std::is_default_constructible<core::SourceSessionEvidence>::value,
              "SourceSessionEvidence must not have a public default constructor");
static_assert(!std::is_constructible<
                  core::SourceSessionEvidence,
                  std::string_view, std::string_view,
                  std::string_view, std::string_view>::value,
              "Application code must not construct evidence from arbitrary strings");
static_assert(std::is_copy_constructible<core::SourceSessionEvidence>::value,
              "Trusted evidence may be passed through the event pipeline");
}  // namespace dockstation::verification
