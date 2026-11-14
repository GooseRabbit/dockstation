#pragma once

#include <cstdint>
#include <string_view>

#include "event_processing.hpp"

namespace dockstation::core {

struct SourceSessionEvidence {
    std::string_view source{};
    std::string_view source_instance{};
    std::string_view peer_identity_evidence{};
    std::string_view session_proof{};
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
