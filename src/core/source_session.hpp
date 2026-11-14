#pragma once

#include <cstdint>
#include <string_view>

#include "event_processing.hpp"

namespace dockstation::communications {
class AuthenticatedEventTransportAdapter;
}
namespace dockstation::verification {
class SourceSessionEvidenceTestAccess;
}

namespace dockstation::core {

// Opaque evidence. Ordinary application/API code cannot construct or mutate it.
// This is an API boundary, not a sandbox against hostile native code in-process.
class SourceSessionEvidence final {
public:
    SourceSessionEvidence(const SourceSessionEvidence&) noexcept = default;
    SourceSessionEvidence& operator=(const SourceSessionEvidence&) noexcept = default;
    SourceSessionEvidence(SourceSessionEvidence&&) noexcept = default;
    SourceSessionEvidence& operator=(SourceSessionEvidence&&) noexcept = default;

    [[nodiscard]] std::string_view source() const noexcept { return source_; }
    [[nodiscard]] std::string_view source_instance() const noexcept { return source_instance_; }
    [[nodiscard]] std::string_view peer_identity_evidence() const noexcept { return peer_identity_evidence_; }
    [[nodiscard]] std::string_view session_proof() const noexcept { return session_proof_; }

private:
    friend class ::dockstation::communications::AuthenticatedEventTransportAdapter;
    friend class ::dockstation::verification::SourceSessionEvidenceTestAccess;

    SourceSessionEvidence(std::string_view source,
                          std::string_view source_instance,
                          std::string_view peer_identity_evidence,
                          std::string_view session_proof) noexcept
        : source_(source), source_instance_(source_instance),
          peer_identity_evidence_(peer_identity_evidence), session_proof_(session_proof) {}

    std::string_view source_{};
    std::string_view source_instance_{};
    std::string_view peer_identity_evidence_{};
    std::string_view session_proof_{};
};

enum class SourceSessionVerificationStatus : std::uint8_t {
    Verified,
    UnknownSource,
    Rejected,
    Unavailable,
    Error
};

// Trusted output of SourceSessionVerifier, not a bearer credential.
// Only the trusted coordinator may pass this value to SourceSessionRepository.
struct VerifiedSourceSession {
    std::string_view source{};
    std::string_view source_instance{};
    std::string_view registration_revision{};
};

struct SourceSessionVerificationResult {
    SourceSessionVerificationStatus status{
        SourceSessionVerificationStatus::Error
    };
    VerifiedSourceSession session{};
};

[[nodiscard]] bool is_valid_source_session_evidence(
    const SourceSessionEvidence& evidence) noexcept;

[[nodiscard]] bool is_valid_verified_source_session(
    const VerifiedSourceSession& session) noexcept;

[[nodiscard]] bool is_valid_source_session_verification_result(
    const SourceSessionEvidence& evidence,
    const SourceSessionVerificationResult& result) noexcept;

class SourceSessionVerifier {
public:
    virtual ~SourceSessionVerifier() = default;

    [[nodiscard]] virtual SourceSessionVerificationResult
    verify_source_session(
        const SourceSessionEvidence& evidence) noexcept = 0;
};

enum class SourceSessionConfirmationStatus : std::uint8_t {
    Confirmed,
    AlreadyCurrent,
    CursorChanged,
    RegistrationChanged,
    Error
};

struct SourceSessionConfirmationResult {
    SourceSessionConfirmationStatus status{
        SourceSessionConfirmationStatus::Error
    };
    SourceCursor cursor{};
};

class SourceSessionRepository {
public:
    virtual ~SourceSessionRepository() = default;

    // Implementations compare the full expected cursor and the current
    // registration state/revision in the same transaction as the cursor update.
    [[nodiscard]] virtual SourceSessionConfirmationResult
    confirm_source_session(
        const SourceCursorReadResult& expected_cursor,
        const VerifiedSourceSession& verified_session) noexcept = 0;
};

[[nodiscard]] bool is_valid_source_session_confirmation_result(
    const SourceCursorReadResult& expected_cursor,
    const VerifiedSourceSession& verified_session,
    const SourceSessionConfirmationResult& result) noexcept;

}  // namespace dockstation::core
