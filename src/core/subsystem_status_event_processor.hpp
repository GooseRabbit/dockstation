#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "event_handling.hpp"

namespace dockstation::core {

// Status values are observations reported by a registered subsystem.
// NOT_CONFIGURED, NOT_APPLICABLE and DEFERRED are configuration/diagnostic
// outcomes and are deliberately not accepted as controller-reported states.
enum class ReportedSubsystemStatus : std::uint8_t {
    Unknown,
    Pass,
    Degraded,
    Fail
};

[[nodiscard]] bool is_known_reported_subsystem_status(
    ReportedSubsystemStatus status) noexcept;

// The core does not parse JSON or bind itself to ControllerLink serialization.
// A trusted decoder converts the event-specific payload into this borrowed view.
struct DecodedSubsystemStatus final {
    std::string_view subsystem_id{};
    ReportedSubsystemStatus status{ReportedSubsystemStatus::Unknown};
    std::string_view error_code{};
    std::string_view severity{};
    std::string_view description{};
    std::optional<std::string_view> reason_text{};
};

enum class SubsystemStatusDecodeStatus : std::uint8_t {
    Decoded,
    UnsupportedPayload,
    Error
};

struct SubsystemStatusDecodeResult final {
    SubsystemStatusDecodeStatus status{SubsystemStatusDecodeStatus::Error};
    DecodedSubsystemStatus report{};
};

class SubsystemStatusEventDecoderPort {
public:
    virtual ~SubsystemStatusEventDecoderPort() = default;

    // Decodes the payload according to its schema version. All returned views
    // must remain valid until process() returns.
    [[nodiscard]] virtual SubsystemStatusDecodeResult decode(
        const Event& event) noexcept = 0;
};

enum class SubsystemRegistrationLookupStatus : std::uint8_t {
    Found,
    NotFound,
    Unavailable,
    Error
};

enum class SubsystemRegistrationState : std::uint8_t {
    Unknown,
    Active,
    Disabled,
    Revoked
};

struct SubsystemRegistrationRecord final {
    std::string_view station_id{};
    std::string_view subsystem_id{};
    std::string_view source{};
    std::string_view registration_revision{};
    SubsystemRegistrationState state{SubsystemRegistrationState::Unknown};
};

struct SubsystemRegistrationLookupResult final {
    SubsystemRegistrationLookupStatus status{
        SubsystemRegistrationLookupStatus::Error
    };
    SubsystemRegistrationRecord record{};
};

// Storage-neutral lookup of an exact subsystem registration under the local
// Station ID. A reader must resolve exactly one record; duplicates or
// conflicting rows are Error. Returned views must outlive process().
class SubsystemRegistrationReader {
public:
    virtual ~SubsystemRegistrationReader() = default;

    [[nodiscard]] virtual SubsystemRegistrationLookupResult find_subsystem(
        std::string_view station_id,
        std::string_view subsystem_id) noexcept = 0;
};

[[nodiscard]] bool is_valid_subsystem_registration_lookup_result(
    const SubsystemRegistrationLookupResult& result) noexcept;

enum class SubsystemStatusCommitStatus : std::uint8_t {
    Committed,
    DuplicateEvent,
    EventConflict,
    SubsystemNotRegistered,
    RegistrationChanged,
    SourceCursorChanged,
    Error
};

struct SubsystemStatusCommitResult final {
    SubsystemStatusCommitStatus status{
        SubsystemStatusCommitStatus::Error
    };
};

// Logical transaction boundary for a subsystem status report. The production
// implementation must atomically:
//  * verify the registration is still ACTIVE and exactly matches the expected
//    station/subsystem/source/revision;
//  * compare every field of expected_cursor with the persisted source cursor;
//  * verify event_id uniqueness (same ID + content is duplicate; changed
//    content is conflict);
//  * persist the report/status and original Event;
//  * advance the source cursor to the Event's source/instance/sequence.
// The adapter must copy all string_view data from the registration, report,
// Event payload and cursor into owned storage before returning.
// No partial write is permitted. This contract does not select a database.
class SubsystemStatusRepository : public EventRepository {
public:
    ~SubsystemStatusRepository() override = default;

    [[nodiscard]] virtual SubsystemStatusCommitResult commit_subsystem_status(
        const SubsystemRegistrationRecord& expected_registration,
        const DecodedSubsystemStatus& report,
        const Event& event,
        const SourceCursor& expected_cursor) noexcept = 0;
};

// Profile processor for SUBSYSTEM_STATUS_UPDATED. It assumes the caller has
// already passed SourceSessionCoordinator/StationEventPipeline authentication
// and ordering gates; it does not authenticate a transport session itself.
// It is intentionally not added to StationEventPipeline until production
// registry and transactional repository adapters are available.
class SubsystemStatusEventProcessor final : public EventProcessorPort {
public:
    // local_station_id storage must outlive this processor.
    SubsystemStatusEventProcessor(
        std::string_view local_station_id,
        SubsystemStatusEventDecoderPort& decoder,
        SubsystemRegistrationReader& registration_reader,
        SubsystemStatusRepository& repository) noexcept;

    [[nodiscard]] bool is_configured() const noexcept;

    [[nodiscard]] EventProcessorStatus process(
        const Event& event,
        EventCategory category,
        const SourceCursor& expected_cursor) noexcept override;

private:
    std::string_view local_station_id_;
    SubsystemStatusEventDecoderPort& decoder_;
    SubsystemRegistrationReader& registration_reader_;
    SubsystemStatusRepository& repository_;
    bool configured_{false};
};

}  // namespace dockstation::core
