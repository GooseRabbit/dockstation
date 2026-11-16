#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/source_session_verifier.hpp"
#include "platform/persistence/sqlite_database.hpp"

namespace dockstation::platform::persistence {

enum class SqliteSourceRegistrationWriteStatus : std::uint8_t {
    Created,
    Updated,
    NotFound,
    RevisionMismatch,
    Conflict,
    InvalidArgument,
    Unavailable,
    Error
};

struct SqliteSourceRegistrationWriteResult final {
    SqliteSourceRegistrationWriteStatus status{
        SqliteSourceRegistrationWriteStatus::Error};
    int sqlite_status{0};

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == SqliteSourceRegistrationWriteStatus::Created ||
               status == SqliteSourceRegistrationWriteStatus::Updated;
    }
};

// Persistent adapters for the trusted source registry and source-session cursor.
// This class does not authenticate a peer or create SourceSessionEvidence: it
// only resolves the opaque identity supplied by the live TLS transport adapter.
// All calls must be serialized by the composition root that owns SqliteDatabase.
// Returned string_views remain valid until the next call to the same reader port.
class SqliteSourceSessionStore final
    : public core::SourceRegistrationReader,
      public core::SourceCursorReader,
      public core::SourceSessionRepository {
public:
    explicit SqliteSourceSessionStore(SqliteDatabase& database) noexcept;

    SqliteSourceSessionStore(const SqliteSourceSessionStore&) = delete;
    SqliteSourceSessionStore& operator=(const SqliteSourceSessionStore&) = delete;
    SqliteSourceSessionStore(SqliteSourceSessionStore&&) = delete;
    SqliteSourceSessionStore& operator=(SqliteSourceSessionStore&&) = delete;

    // Trusted local provisioning operation. The identity is public opaque
    // metadata, not the PSK secret. No HTTP/API route exposes this method.
    [[nodiscard]] SqliteSourceRegistrationWriteResult create_source_registration(
        std::string_view station_id,
        std::string_view source,
        std::string_view registration_revision,
        std::string_view peer_identity) noexcept;

    // Changes only state/revision. A REVOKED registration is terminal and
    // cannot be reactivated; create a new registration with a new identity.
    [[nodiscard]] SqliteSourceRegistrationWriteResult change_source_state(
        std::string_view source,
        std::string_view expected_revision,
        std::string_view new_revision,
        core::SourceRegistrationState next_state) noexcept;

    [[nodiscard]] core::SourceRegistrationLookupResult find_by_peer_identity(
        std::string_view peer_identity_evidence) noexcept override;

    [[nodiscard]] core::SourceCursorReadResult read_source_cursor(
        std::string_view source) noexcept override;

    [[nodiscard]] core::SourceSessionConfirmationResult confirm_source_session(
        const core::SourceCursorReadResult& expected_cursor,
        const core::VerifiedSourceSession& verified_session) noexcept override;

private:
    SqliteDatabase& database_;

    // Registry, expected-cursor and confirmation-result buffers are separate.
    // SourceSessionCoordinator validates the returned confirmation against the
    // expected cursor after this method returns; mutating the reader's cursor
    // buffer here would silently rewrite that expected snapshot.
    std::string registration_station_id_;
    std::string registration_source_;
    std::string registration_revision_;
    std::string cursor_source_;
    std::string cursor_instance_;
    std::string confirmation_source_;
    std::string confirmation_instance_;
};

}  // namespace dockstation::platform::persistence
