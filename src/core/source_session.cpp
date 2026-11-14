#include "source_session.hpp"

namespace dockstation::core {
namespace {

bool has_non_whitespace(std::string_view value) noexcept {
    for (const char ch : value) {
        if (ch != ' ' && ch != '\t' && ch != '\n' &&
            ch != '\r' && ch != '\f' && ch != '\v') {
            return true;
        }
    }
    return false;
}

bool is_empty_session(const VerifiedSourceSession& session) noexcept {
    return session.source.empty() &&
           session.source_instance.empty() &&
           session.registration_revision.empty();
}

bool is_empty_cursor(const SourceCursor& cursor) noexcept {
    return cursor.source.empty() &&
           !cursor.source_instance.has_value() &&
           !cursor.last_sequence.has_value();
}

bool same_cursor(const SourceCursor& left,
                 const SourceCursor& right) noexcept {
    return left.source == right.source &&
           left.source_instance.has_value() ==
               right.source_instance.has_value() &&
           (!left.source_instance.has_value() ||
            *left.source_instance == *right.source_instance) &&
           left.last_sequence == right.last_sequence;
}

bool same_session(const SourceCursorReadResult& expected,
                  const VerifiedSourceSession& verified) noexcept {
    return expected.status == SourceCursorReadStatus::Found &&
           expected.cursor.source_instance.has_value() &&
           *expected.cursor.source_instance == verified.source_instance;
}

}  // namespace

bool is_valid_source_session_evidence(
    const SourceSessionEvidence& evidence) noexcept {
    // Identifiers are text and must contain a non-whitespace character.
    // Proof/evidence are opaque byte strings: only their presence is required.
    return has_non_whitespace(evidence.source()) &&
           has_non_whitespace(evidence.source_instance()) &&
           !evidence.peer_identity_evidence().empty() &&
           !evidence.session_proof().empty();
}

bool is_valid_verified_source_session(
    const VerifiedSourceSession& session) noexcept {
    return has_non_whitespace(session.source) &&
           has_non_whitespace(session.source_instance) &&
           has_non_whitespace(session.registration_revision);
}

bool is_valid_source_session_verification_result(
    const SourceSessionEvidence& evidence,
    const SourceSessionVerificationResult& result) noexcept {
    if (!is_valid_source_session_evidence(evidence)) {
        return false;
    }

    switch (result.status) {
        case SourceSessionVerificationStatus::Verified:
            return is_valid_verified_source_session(result.session) &&
                   result.session.source == evidence.source() &&
                   result.session.source_instance == evidence.source_instance();
        case SourceSessionVerificationStatus::UnknownSource:
        case SourceSessionVerificationStatus::Rejected:
        case SourceSessionVerificationStatus::Unavailable:
        case SourceSessionVerificationStatus::Error:
            return is_empty_session(result.session);
    }
    return false;
}

bool is_valid_source_session_confirmation_result(
    const SourceCursorReadResult& expected_cursor,
    const VerifiedSourceSession& verified_session,
    const SourceSessionConfirmationResult& result) noexcept {
    if (!is_valid_verified_source_session(verified_session) ||
        !is_valid_source_cursor_read_result(verified_session.source,
                                            expected_cursor) ||
        (expected_cursor.status != SourceCursorReadStatus::Found &&
         expected_cursor.status != SourceCursorReadStatus::NotFound)) {
        return false;
    }

    switch (result.status) {
        case SourceSessionConfirmationStatus::Confirmed: {
            const bool new_session =
                expected_cursor.status == SourceCursorReadStatus::NotFound ||
                !same_session(expected_cursor, verified_session);
            return new_session &&
                   result.cursor.source == verified_session.source &&
                   result.cursor.source_instance.has_value() &&
                   *result.cursor.source_instance ==
                       verified_session.source_instance &&
                   !result.cursor.last_sequence.has_value();
        }
        case SourceSessionConfirmationStatus::AlreadyCurrent:
            return same_session(expected_cursor, verified_session) &&
                   same_cursor(result.cursor, expected_cursor.cursor);
        case SourceSessionConfirmationStatus::CursorChanged:
        case SourceSessionConfirmationStatus::RegistrationChanged:
        case SourceSessionConfirmationStatus::Error:
            return is_empty_cursor(result.cursor);
    }
    return false;
}

}  // namespace dockstation::core
