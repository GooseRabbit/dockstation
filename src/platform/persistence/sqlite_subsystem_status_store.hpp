#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/subsystem_status_event_processor.hpp"
#include "platform/persistence/sqlite_database.hpp"

namespace dockstation::platform::persistence {

enum class SqliteSubsystemRegistrationWriteStatus : std::uint8_t {
    Created,
    Updated,
    NotFound,
    RevisionMismatch,
    SourceNotRegistered,
    Conflict,
    InvalidArgument,
    Unavailable,
    Error
};

struct SqliteSubsystemRegistrationWriteResult final {
    SqliteSubsystemRegistrationWriteStatus status{
        SqliteSubsystemRegistrationWriteStatus::Error};
    int sqlite_status{0};

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == SqliteSubsystemRegistrationWriteStatus::Created ||
               status == SqliteSubsystemRegistrationWriteStatus::Updated;
    }
};

// Durable subsystem registry and status repository. It never authenticates a
// transport or creates SourceSessionEvidence. Calls must be serialized by the
// Gateway composition root that owns SqliteDatabase.
class SqliteSubsystemStatusStore final
    : public core::SubsystemRegistrationReader,
      public core::SubsystemStatusRepository {
public:
    explicit SqliteSubsystemStatusStore(SqliteDatabase& database) noexcept;
    SqliteSubsystemStatusStore(const SqliteSubsystemStatusStore&) = delete;
    SqliteSubsystemStatusStore& operator=(const SqliteSubsystemStatusStore&) = delete;
    SqliteSubsystemStatusStore(SqliteSubsystemStatusStore&&) = delete;
    SqliteSubsystemStatusStore& operator=(SqliteSubsystemStatusStore&&) = delete;

    // Trusted local provisioning boundary. Source must already be ACTIVE and
    // registered to the same Station ID. No HTTP provisioning route is exposed.
    [[nodiscard]] SqliteSubsystemRegistrationWriteResult create_subsystem_registration(
        std::string_view station_id,
        std::string_view subsystem_id,
        std::string_view source,
        std::string_view registration_revision) noexcept;

    // Revision-checked state change. REVOKED is terminal.
    [[nodiscard]] SqliteSubsystemRegistrationWriteResult change_subsystem_state(
        std::string_view station_id,
        std::string_view subsystem_id,
        std::string_view expected_revision,
        std::string_view new_revision,
        core::SubsystemRegistrationState next_state) noexcept;

    [[nodiscard]] core::SubsystemRegistrationLookupResult find_subsystem(
        std::string_view station_id,
        std::string_view subsystem_id) noexcept override;

    [[nodiscard]] core::EventRepositoryLookupResult find_by_id(
        std::string_view event_id) noexcept override;

    [[nodiscard]] core::SubsystemStatusCommitResult commit_subsystem_status(
        const core::SubsystemRegistrationRecord& expected_registration,
        const core::DecodedSubsystemStatus& report,
        const core::Event& event,
        const core::SourceCursor& expected_cursor) noexcept override;

private:
    struct EventBuffer final {
        std::string event_id;
        std::string event_type;
        std::string source;
        std::string received_at;
        std::string quality_status;
        std::string schema_version;
        std::optional<std::string> source_instance;
        std::optional<std::string> occurred_at;
        std::optional<std::string> operation_id;
        std::optional<std::string> correlation_id;
        std::optional<std::string> payload;
        std::optional<std::string> reason_code;
        std::optional<std::string> reason_text;
        std::optional<std::uint64_t> source_sequence;
    };

    SqliteDatabase& database_;
    std::string registration_station_id_;
    std::string registration_subsystem_id_;
    std::string registration_source_;
    std::string registration_revision_;
    EventBuffer event_buffer_{};
    core::Event cached_event_{};
};

}  // namespace dockstation::platform::persistence
