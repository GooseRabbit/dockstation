#pragma once

#include "event.hpp"
#include "source_session.hpp"

namespace dockstation::core {

enum class SourceSessionCoordinatorStatus : std::uint8_t {
    Confirmed,
    AlreadyCurrent,
    InvalidEvidence,
    UnknownSource,
    Rejected,
    Unavailable,
    VerificationError,
    CursorReadError,
    CursorChanged,
    RegistrationChanged,
    RepositoryError
};

struct SourceSessionCoordinatorResult {
    SourceSessionCoordinatorStatus status{
        SourceSessionCoordinatorStatus::VerificationError
    };
    SourceCursor cursor{};
};

// Выполняет единый fail-closed порядок:
// event/evidence binding -> verifier -> cursor reader -> transactional repository.
// Зависимости не владеются и должны жить дольше координатора. Строки в возвращённом
// cursor сохраняют контракт времени жизни соответствующего адаптера.
class SourceSessionCoordinator final {
public:
    SourceSessionCoordinator(
        SourceSessionVerifier& verifier,
        SourceCursorReader& cursor_reader,
        SourceSessionRepository& repository) noexcept;

    [[nodiscard]] SourceSessionCoordinatorResult confirm(
        const Event& event,
        const SourceSessionEvidence& evidence) noexcept;

private:
    SourceSessionVerifier& verifier_;
    SourceCursorReader& cursor_reader_;
    SourceSessionRepository& repository_;
};

}  // namespace dockstation::core
