#include "subsystem_status_event_processor.hpp"

#include "event_processing.hpp"
#include "event_validation.hpp"

namespace dockstation::core {
namespace {

bool has_non_whitespace(std::string_view value) noexcept {
    for (const char character : value) {
        if (character != ' ' && character != '\t' && character != '\n' &&
            character != '\r' && character != '\f' && character != '\v') {
            return true;
        }
    }
    return false;
}

bool is_empty_registration(
    const SubsystemRegistrationRecord& registration) noexcept {
    return registration.station_id.empty() &&
           registration.subsystem_id.empty() &&
           registration.source.empty() &&
           registration.registration_revision.empty() &&
           registration.state == SubsystemRegistrationState::Unknown;
}

bool is_known_registration_state(
    SubsystemRegistrationState state) noexcept {
    return state == SubsystemRegistrationState::Active ||
           state == SubsystemRegistrationState::Disabled ||
           state == SubsystemRegistrationState::Revoked;
}

bool is_known_severity(std::string_view severity) noexcept {
    return severity == "INFO" || severity == "WARNING" ||
           severity == "ERROR" || severity == "CRITICAL";
}

bool is_valid_decoded_report(
    const DecodedSubsystemStatus& report,
    const Event& event) noexcept {
    if (!has_non_whitespace(report.subsystem_id) ||
        !has_non_whitespace(report.error_code) ||
        !has_non_whitespace(report.description) ||
        !is_known_severity(report.severity) ||
        !is_known_reported_subsystem_status(report.status)) {
        return false;
    }

    if (report.reason_text.has_value() != event.reason_text.has_value()) {
        return false;
    }
    if (report.reason_text.has_value()) {
        if (!has_non_whitespace(*report.reason_text) ||
            !event.reason_code.has_value() ||
            *event.reason_code != report.error_code ||
            *event.reason_text != *report.reason_text) {
            return false;
        }
    } else if (event.reason_code.has_value()) {
        return false;
    }

    return true;
}

EventProcessorStatus map_commit_result(
    SubsystemStatusCommitStatus status) noexcept {
    switch (status) {
        // This profile updates subsystem state only. The global StationState
        // is never changed by a subsystem status report.
        case SubsystemStatusCommitStatus::Committed:
            return EventProcessorStatus::NoStateChange;
        case SubsystemStatusCommitStatus::DuplicateEvent:
            return EventProcessorStatus::Duplicate;
        case SubsystemStatusCommitStatus::EventConflict:
            return EventProcessorStatus::EventConflict;
        case SubsystemStatusCommitStatus::SubsystemNotRegistered:
            return EventProcessorStatus::Rejected;
        case SubsystemStatusCommitStatus::RegistrationChanged:
        case SubsystemStatusCommitStatus::SourceCursorChanged:
            return EventProcessorStatus::ConcurrentUpdate;
        case SubsystemStatusCommitStatus::Error:
            return EventProcessorStatus::Error;
    }
    return EventProcessorStatus::Error;
}

}  // namespace

bool is_known_reported_subsystem_status(
    ReportedSubsystemStatus status) noexcept {
    switch (status) {
        case ReportedSubsystemStatus::Unknown:
        case ReportedSubsystemStatus::Pass:
        case ReportedSubsystemStatus::Degraded:
        case ReportedSubsystemStatus::Fail:
            return true;
    }
    return false;
}

bool is_valid_subsystem_registration_lookup_result(
    const SubsystemRegistrationLookupResult& result) noexcept {
    switch (result.status) {
        case SubsystemRegistrationLookupStatus::Found:
            return has_non_whitespace(result.record.station_id) &&
                   has_non_whitespace(result.record.subsystem_id) &&
                   has_non_whitespace(result.record.source) &&
                   has_non_whitespace(result.record.registration_revision) &&
                   is_known_registration_state(result.record.state);
        case SubsystemRegistrationLookupStatus::NotFound:
        case SubsystemRegistrationLookupStatus::Unavailable:
        case SubsystemRegistrationLookupStatus::Error:
            return is_empty_registration(result.record);
    }
    return false;
}

SubsystemStatusEventProcessor::SubsystemStatusEventProcessor(
    std::string_view local_station_id,
    SubsystemStatusEventDecoderPort& decoder,
    SubsystemRegistrationReader& registration_reader,
    SubsystemStatusRepository& repository) noexcept
    : local_station_id_(local_station_id),
      decoder_(decoder),
      registration_reader_(registration_reader),
      repository_(repository),
      configured_(has_non_whitespace(local_station_id)) {}

bool SubsystemStatusEventProcessor::is_configured() const noexcept {
    return configured_;
}

EventProcessorStatus SubsystemStatusEventProcessor::process(
    const Event& event,
    EventCategory category,
    const SourceCursor& expected_cursor) noexcept {
    // The processor accepts only its exact F13 route. It does not broaden the
    // event catalogue or handle other subsystem event types.
    if (category != EventCategory::Subsystem ||
        event.event_type != "SUBSYSTEM_STATUS_UPDATED") {
        return EventProcessorStatus::Unsupported;
    }

    if (!configured_) {
        return EventProcessorStatus::Error;
    }

    // ControllerLink projector emits schema 1. The source instance and
    // sequence are mandatory because this update replaces a stored observation.
    const EventValidationPolicy policy{true, true, false};
    if (!validate_event(event, policy).valid ||
        event.quality_status != "VALID" ||
        event.schema_version != "1") {
        return EventProcessorStatus::Rejected;
    }

    // F03 is read-only; the repository repeats this check atomically at commit
    // to close the race between lookup and write.
    const DuplicateEventResult duplicate =
        check_duplicate_event(event, repository_);
    switch (duplicate.status) {
        case DuplicateEventStatus::New:
            break;
        case DuplicateEventStatus::Duplicate:
            return EventProcessorStatus::Duplicate;
        case DuplicateEventStatus::Conflict:
            return EventProcessorStatus::EventConflict;
        case DuplicateEventStatus::RepositoryError:
            return EventProcessorStatus::Error;
        default:
            return EventProcessorStatus::Error;
    }

    // Sequence order, not wall-clock timestamps, determines which observation
    // may replace the current status. F13 performs the same preflight; this
    // second check protects direct calls and narrows stale-write exposure.
    const EventFreshnessResult freshness =
        check_event_freshness(event, expected_cursor);
    if (freshness.status != EventFreshnessStatus::Current) {
        return EventProcessorStatus::Error;
    }

    const SubsystemStatusDecodeResult decoded = decoder_.decode(event);
    switch (decoded.status) {
        case SubsystemStatusDecodeStatus::Decoded:
            break;
        case SubsystemStatusDecodeStatus::UnsupportedPayload:
            return EventProcessorStatus::Rejected;
        case SubsystemStatusDecodeStatus::Error:
            return EventProcessorStatus::Error;
        default:
            return EventProcessorStatus::Error;
    }

    if (!is_valid_decoded_report(decoded.report, event)) {
        return EventProcessorStatus::Rejected;
    }

    const SubsystemRegistrationLookupResult lookup =
        registration_reader_.find_subsystem(
            local_station_id_, decoded.report.subsystem_id);
    if (!is_valid_subsystem_registration_lookup_result(lookup)) {
        return EventProcessorStatus::Error;
    }

    switch (lookup.status) {
        case SubsystemRegistrationLookupStatus::NotFound:
            return EventProcessorStatus::Rejected;
        case SubsystemRegistrationLookupStatus::Unavailable:
        case SubsystemRegistrationLookupStatus::Error:
            return EventProcessorStatus::Error;
        case SubsystemRegistrationLookupStatus::Found:
            break;
    }

    const SubsystemRegistrationRecord& registration = lookup.record;
    if (registration.station_id != local_station_id_ ||
        registration.subsystem_id != decoded.report.subsystem_id ||
        registration.source != event.source) {
        return EventProcessorStatus::Rejected;
    }

    switch (registration.state) {
        case SubsystemRegistrationState::Active:
            break;
        case SubsystemRegistrationState::Disabled:
        case SubsystemRegistrationState::Revoked:
            return EventProcessorStatus::Rejected;
        case SubsystemRegistrationState::Unknown:
            return EventProcessorStatus::Error;
    }

    // The repository must atomically re-check the ACTIVE registration revision,
    // event ID and complete source cursor before persisting any domain change.
    const SubsystemStatusCommitResult committed =
        repository_.commit_subsystem_status(
            registration, decoded.report, event, expected_cursor);
    return map_commit_result(committed.status);
}

}  // namespace dockstation::core
