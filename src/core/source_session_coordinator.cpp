#include "source_session_coordinator.hpp"

namespace dockstation::core {
namespace {

bool event_matches_evidence(const Event& event,
                            const SourceSessionEvidence& evidence) noexcept {
    return event.source == evidence.source() &&
           event.source_instance.has_value() &&
           *event.source_instance == evidence.source_instance();
}

SourceSessionCoordinatorResult failure(
    SourceSessionCoordinatorStatus status) noexcept {
    return {status, {}};
}

}  // namespace

SourceSessionCoordinator::SourceSessionCoordinator(
    SourceSessionVerifier& verifier,
    SourceCursorReader& cursor_reader,
    SourceSessionRepository& repository) noexcept
    : verifier_(verifier),
      cursor_reader_(cursor_reader),
      repository_(repository) {}

SourceSessionCoordinatorResult SourceSessionCoordinator::confirm(
    const Event& event,
    const SourceSessionEvidence& evidence) noexcept {
    // До обращения к портам проверяем обязательные поля и привязку Event
    // к тем же source/source_instance, для которых предъявлено доказательство.
    if (!is_valid_source_session_evidence(evidence) ||
        !event_matches_evidence(event, evidence)) {
        return failure(SourceSessionCoordinatorStatus::InvalidEvidence);
    }

    // Внешний verifier — единственный источник VerifiedSourceSession.
    const SourceSessionVerificationResult verification =
        verifier_.verify_source_session(evidence);
    if (!is_valid_source_session_verification_result(evidence, verification)) {
        return failure(SourceSessionCoordinatorStatus::VerificationError);
    }

    switch (verification.status) {
        case SourceSessionVerificationStatus::Verified:
            break;
        case SourceSessionVerificationStatus::UnknownSource:
            return failure(SourceSessionCoordinatorStatus::UnknownSource);
        case SourceSessionVerificationStatus::Rejected:
            return failure(SourceSessionCoordinatorStatus::Rejected);
        case SourceSessionVerificationStatus::Unavailable:
            return failure(SourceSessionCoordinatorStatus::Unavailable);
        case SourceSessionVerificationStatus::Error:
            return failure(SourceSessionCoordinatorStatus::VerificationError);
    }

    // Ошибка чтения не превращается в NotFound: структурно корректный Error
    // также блокирует фиксацию новой сессии.
    const SourceCursorReadResult expected_cursor =
        cursor_reader_.read_source_cursor(verification.session.source);
    if (!is_valid_source_cursor_read_result(verification.session.source,
                                            expected_cursor) ||
        expected_cursor.status == SourceCursorReadStatus::Error) {
        return failure(SourceSessionCoordinatorStatus::CursorReadError);
    }

    // Только доверенный результат verifier-а передаётся в транзакционный repository.
    const SourceSessionConfirmationResult confirmation =
        repository_.confirm_source_session(expected_cursor, verification.session);
    if (!is_valid_source_session_confirmation_result(
            expected_cursor, verification.session, confirmation)) {
        return failure(SourceSessionCoordinatorStatus::RepositoryError);
    }

    switch (confirmation.status) {
        case SourceSessionConfirmationStatus::Confirmed:
            return {SourceSessionCoordinatorStatus::Confirmed,
                    confirmation.cursor};
        case SourceSessionConfirmationStatus::AlreadyCurrent:
            return {SourceSessionCoordinatorStatus::AlreadyCurrent,
                    confirmation.cursor};
        case SourceSessionConfirmationStatus::CursorChanged:
            return failure(SourceSessionCoordinatorStatus::CursorChanged);
        case SourceSessionConfirmationStatus::RegistrationChanged:
            return failure(SourceSessionCoordinatorStatus::RegistrationChanged);
        case SourceSessionConfirmationStatus::Error:
            return failure(SourceSessionCoordinatorStatus::RepositoryError);
    }

    return failure(SourceSessionCoordinatorStatus::RepositoryError);
}

}  // namespace dockstation::core
