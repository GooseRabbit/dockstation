#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "source_session.hpp"

namespace dockstation::core {

enum class SourceRegistrationLookupStatus : std::uint8_t {
    Found,
    NotFound,
    Unavailable,
    Error
};

enum class SourceRegistrationState : std::uint8_t {
    Unknown,
    Active,
    Disabled,
    Revoked
};

struct SourceRegistrationRecord final {
    std::string_view station_id{};
    std::string_view source{};
    std::string_view registration_revision{};
    SourceRegistrationState state{SourceRegistrationState::Unknown};
};

struct SourceRegistrationLookupResult final {
    SourceRegistrationLookupStatus status{
        SourceRegistrationLookupStatus::Error
    };
    SourceRegistrationRecord record{};
};

// Read-only, storage-neutral lookup. The identity is opaque binary evidence
// (currently the pair-specific PSK identity from DEC-066), not a Station ID,
// source name, or secret PSK. The production adapter must obtain it from the
// local Repka credential used by the completed TLS handshake, never from an
// arbitrary peer message. A reader must resolve it to exactly one record;
// duplicate/conflicting registrations are an Error. Returned views must
// remain valid until the enclosing SourceSessionCoordinator::confirm call
// completes.
class SourceRegistrationReader {
public:
    virtual ~SourceRegistrationReader() = default;

    [[nodiscard]] virtual SourceRegistrationLookupResult
    find_by_peer_identity(
        std::string_view peer_identity_evidence) noexcept = 0;
};

[[nodiscard]] bool is_valid_source_registration_lookup_result(
    const SourceRegistrationLookupResult& result) noexcept;

// Applies registry policy without selecting a database or implementing
// cryptography. The transport adapter must construct evidence only while the
// matching authenticated channel is live.
class RegistrySourceSessionVerifier final : public SourceSessionVerifier {
public:
    // local_station_id storage must outlive this verifier.
    RegistrySourceSessionVerifier(
        SourceRegistrationReader& registry,
        std::string_view local_station_id) noexcept;

    [[nodiscard]] SourceSessionVerificationResult verify_source_session(
        const SourceSessionEvidence& evidence) noexcept override;

private:
    SourceRegistrationReader& registry_;
    std::string_view local_station_id_;
    bool configured_{false};
};

}  // namespace dockstation::core
