#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

#include "core/subsystem_status_event_processor.hpp"

using namespace dockstation::core;

namespace {

int checks = 0;
int failures = 0;

void expect(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << description << '\n';
    } else {
        std::cerr << "FAIL - " << description << '\n';
        ++failures;
    }
}

constexpr std::string_view kPayload =
    R"({"subsystem_id":"weather.temperature","status":"DEGRADED"})";

Event make_event(
    std::string_view event_id = "event-1",
    std::optional<std::string_view> source_instance =
        std::string_view{"boot-1"},
    std::optional<std::uint64_t> sequence = 1U,
    std::string_view quality = "VALID",
    std::string_view schema_version = "1",
    std::optional<std::string_view> reason_code =
        std::string_view{"DHT22_READ_FAILED"},
    std::optional<std::string_view> reason_text =
        std::string_view{"I2C timeout"}) noexcept {
    return {
        event_id,
        "SUBSYSTEM_STATUS_UPDATED",
        "esp32.main",
        source_instance,
        std::nullopt,
        "received-at",
        sequence,
        std::nullopt,
        std::nullopt,
        kPayload,
        quality,
        schema_version,
        reason_code,
        reason_text
    };
}

SourceCursor make_cursor(std::uint64_t last_sequence = 0U) noexcept {
    return {"esp32.main", std::string_view{"boot-1"}, last_sequence};
}

DecodedSubsystemStatus make_report() noexcept {
    return {
        "weather.temperature",
        ReportedSubsystemStatus::Degraded,
        "DHT22_READ_FAILED",
        "WARNING",
        "DHT22 read failed",
        std::string_view{"I2C timeout"}
    };
}

struct FakeDecoder final : SubsystemStatusEventDecoderPort {
    SubsystemStatusDecodeResult result{
        SubsystemStatusDecodeStatus::Decoded, make_report()};
    int calls{0};

    SubsystemStatusDecodeResult decode(const Event&) noexcept override {
        ++calls;
        return result;
    }
};

struct FakeRegistrationReader final : SubsystemRegistrationReader {
    SubsystemRegistrationLookupResult result{
        SubsystemRegistrationLookupStatus::Found,
        {"station-1", "weather.temperature", "esp32.main",
         "registration-rev-7", SubsystemRegistrationState::Active}};
    int calls{0};
    std::string_view requested_station{};
    std::string_view requested_subsystem{};

    SubsystemRegistrationLookupResult find_subsystem(
        std::string_view station_id,
        std::string_view subsystem_id) noexcept override {
        ++calls;
        requested_station = station_id;
        requested_subsystem = subsystem_id;
        return result;
    }
};

struct FakeRepository final : SubsystemStatusRepository {
    EventRepositoryLookupStatus lookup_status{
        EventRepositoryLookupStatus::NotFound};
    Event stored_event{};
    SubsystemStatusCommitResult commit_result{
        SubsystemStatusCommitStatus::Committed};
    int lookup_calls{0};
    int commit_calls{0};
    SubsystemRegistrationRecord committed_registration{};
    DecodedSubsystemStatus committed_report{};
    Event committed_event{};
    SourceCursor committed_cursor{};

    EventRepositoryLookupResult find_by_id(
        std::string_view) noexcept override {
        ++lookup_calls;
        if (lookup_status == EventRepositoryLookupStatus::Found) {
            return {lookup_status, stored_event};
        }
        return {lookup_status, {}};
    }

    SubsystemStatusCommitResult commit_subsystem_status(
        const SubsystemRegistrationRecord& registration,
        const DecodedSubsystemStatus& report,
        const Event& event,
        const SourceCursor& expected_cursor) noexcept override {
        ++commit_calls;
        committed_registration = registration;
        committed_report = report;
        committed_event = event;
        committed_cursor = expected_cursor;
        return commit_result;
    }
};

struct Fixture final {
    std::string_view station_id{"station-1"};
    FakeDecoder decoder{};
    FakeRegistrationReader registration{};
    FakeRepository repository{};
    SubsystemStatusEventProcessor processor;

    Fixture() noexcept
        : processor(station_id, decoder, registration, repository) {}
};

EventProcessorStatus process(
    Fixture& fixture,
    const Event& event = make_event(),
    EventCategory category = EventCategory::Subsystem,
    const SourceCursor& cursor = make_cursor()) noexcept {
    return fixture.processor.process(event, category, cursor);
}

}  // namespace

int main() {
    {
        Fixture fixture;
        expect(fixture.processor.is_configured(),
               "non-empty local Station ID configures the processor");
    }
    {
        FakeDecoder decoder;
        FakeRegistrationReader registration;
        FakeRepository repository;
        const std::string_view empty_station{};
        SubsystemStatusEventProcessor processor(
            empty_station, decoder, registration, repository);
        expect(!processor.is_configured() &&
                   processor.process(make_event(), EventCategory::Subsystem,
                                     make_cursor()) == EventProcessorStatus::Error,
               "empty local Station ID fails closed");
    }
    {
        Fixture fixture;
        Event event = make_event();
        event.event_type = "SELF_TEST_COMPLETED";
        expect(process(fixture, event) == EventProcessorStatus::Unsupported &&
                   fixture.repository.lookup_calls == 0 &&
                   fixture.decoder.calls == 0 &&
                   fixture.registration.calls == 0,
               "other event types are not handled by this processor");
    }
    {
        Fixture fixture;
        expect(process(fixture, make_event(), EventCategory::Operation) ==
                   EventProcessorStatus::Unsupported &&
                   fixture.repository.lookup_calls == 0,
               "other event categories are not handled");
    }
    {
        Fixture fixture;
        Event event = make_event("event-no-instance", std::nullopt);
        expect(process(fixture, event) == EventProcessorStatus::Rejected &&
                   fixture.repository.lookup_calls == 0,
               "missing source instance is rejected before repository access");
    }
    {
        Fixture fixture;
        Event event = make_event("event-no-sequence",
                                 std::string_view{"boot-1"}, std::nullopt);
        expect(process(fixture, event) == EventProcessorStatus::Rejected &&
                   fixture.repository.lookup_calls == 0,
               "missing source sequence is rejected before repository access");
    }
    {
        Fixture fixture;
        Event event = make_event("event-unreliable",
                                 std::string_view{"boot-1"}, 1U, "UNKNOWN");
        expect(process(fixture, event) == EventProcessorStatus::Rejected &&
                   fixture.repository.lookup_calls == 0,
               "non-VALID event quality cannot update subsystem state");
    }
    {
        Fixture fixture;
        Event event = make_event("event-schema", std::string_view{"boot-1"},
                                 1U, "VALID", "2");
        expect(process(fixture, event) == EventProcessorStatus::Rejected &&
                   fixture.repository.lookup_calls == 0,
               "unsupported schema version is rejected");
    }
    {
        Fixture fixture;
        fixture.repository.lookup_status = EventRepositoryLookupStatus::Found;
        fixture.repository.stored_event = make_event();
        expect(process(fixture) == EventProcessorStatus::Duplicate &&
                   fixture.decoder.calls == 0 &&
                   fixture.registration.calls == 0 &&
                   fixture.repository.commit_calls == 0,
               "exact event replay is stopped before decoding and commit");
    }
    {
        Fixture fixture;
        fixture.repository.lookup_status = EventRepositoryLookupStatus::Found;
        fixture.repository.stored_event = make_event(
            "event-1", std::string_view{"boot-1"}, 1U,
            "VALID", "1", std::string_view{"OTHER_ERROR"},
            std::string_view{"Different reason"});
        expect(process(fixture) == EventProcessorStatus::EventConflict &&
                   fixture.decoder.calls == 0 &&
                   fixture.repository.commit_calls == 0,
               "reused event ID with different content is a conflict");
    }
    {
        Fixture fixture;
        fixture.repository.lookup_status = EventRepositoryLookupStatus::Error;
        expect(process(fixture) == EventProcessorStatus::Error &&
                   fixture.decoder.calls == 0 &&
                   fixture.repository.commit_calls == 0,
               "event repository read error blocks processing");
    }
    {
        Fixture fixture;
        expect(process(fixture, make_event(), EventCategory::Subsystem,
                       make_cursor(1U)) == EventProcessorStatus::Error &&
                   fixture.decoder.calls == 0,
               "repeated source sequence is unordered and rejected");
    }
    {
        Fixture fixture;
        expect(process(fixture, make_event(), EventCategory::Subsystem,
                       make_cursor(2U)) == EventProcessorStatus::Error &&
                   fixture.decoder.calls == 0,
               "regressed source sequence is rejected");
    }
    {
        Fixture fixture;
        const SourceCursor wrong_source{
            "another.source", std::string_view{"boot-1"}, 0U};
        expect(process(fixture, make_event(), EventCategory::Subsystem,
                       wrong_source) == EventProcessorStatus::Error &&
                   fixture.decoder.calls == 0,
               "source cursor must belong to the exact event source");
    }
    {
        Fixture fixture;
        const SourceCursor wrong_instance{
            "esp32.main", std::string_view{"old-boot"}, 0U};
        expect(process(fixture, make_event(), EventCategory::Subsystem,
                       wrong_instance) == EventProcessorStatus::Error &&
                   fixture.decoder.calls == 0,
               "source instance mismatch cannot update a subsystem");
    }
    {
        Fixture fixture;
        fixture.decoder.result.status =
            SubsystemStatusDecodeStatus::UnsupportedPayload;
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.registration.calls == 0 &&
                   fixture.repository.commit_calls == 0,
               "unsupported payload schema is rejected");
    }
    {
        Fixture fixture;
        fixture.decoder.result.status = SubsystemStatusDecodeStatus::Error;
        expect(process(fixture) == EventProcessorStatus::Error &&
                   fixture.registration.calls == 0 &&
                   fixture.repository.commit_calls == 0,
               "decoder error blocks registration lookup and commit");
    }
    {
        Fixture fixture;
        fixture.decoder.result.status =
            static_cast<SubsystemStatusDecodeStatus>(255U);
        expect(process(fixture) == EventProcessorStatus::Error &&
                   fixture.registration.calls == 0 &&
                   fixture.repository.commit_calls == 0,
               "unknown decoder result fails closed");
    }
    {
        Fixture fixture;
        fixture.decoder.result.report.subsystem_id = " \t";
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.registration.calls == 0,
               "blank subsystem ID is rejected");
    }
    {
        Fixture fixture;
        fixture.decoder.result.report.status =
            static_cast<ReportedSubsystemStatus>(255U);
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.registration.calls == 0,
               "unknown subsystem status enum is rejected");
    }
    {
        Fixture fixture;
        fixture.decoder.result.report.status = ReportedSubsystemStatus::Unknown;
        fixture.repository.commit_result =
            {SubsystemStatusCommitStatus::Committed};
        expect(process(fixture) == EventProcessorStatus::NoStateChange &&
                   fixture.repository.committed_report.status ==
                       ReportedSubsystemStatus::Unknown,
               "explicit UNKNOWN is retained without changing global station state");
    }
    {
        Fixture fixture;
        fixture.decoder.result.report.status = ReportedSubsystemStatus::Pass;
        fixture.decoder.result.report.severity = "CRITICAL";
        expect(process(fixture) == EventProcessorStatus::NoStateChange &&
                   fixture.repository.committed_report.status ==
                       ReportedSubsystemStatus::Pass &&
                   fixture.repository.committed_report.severity == "CRITICAL",
               "explicit PASS is preserved independently from CRITICAL severity");
    }
    {
        Fixture fixture;
        fixture.decoder.result.report.status = ReportedSubsystemStatus::Fail;
        fixture.decoder.result.report.severity = "INFO";
        expect(process(fixture) == EventProcessorStatus::NoStateChange &&
                   fixture.repository.committed_report.status ==
                       ReportedSubsystemStatus::Fail &&
                   fixture.repository.committed_report.severity == "INFO",
               "explicit FAIL is preserved independently from INFO severity");
    }
    {
        Fixture fixture;
        fixture.decoder.result.report.severity = "FATAL";
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.registration.calls == 0,
               "unknown severity is rejected independently from subsystem status");
    }
    {
        Fixture fixture;
        fixture.decoder.result.report.error_code = "";
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.registration.calls == 0,
               "empty error code is rejected");
    }
    {
        Fixture fixture;
        fixture.decoder.result.report.description = " ";
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.registration.calls == 0,
               "empty description is rejected");
    }
    {
        Fixture fixture;
        fixture.decoder.result.report.reason_text = std::nullopt;
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.registration.calls == 0,
               "payload and normalized event reason must agree");
    }
    {
        Fixture fixture;
        fixture.registration.result = {
            SubsystemRegistrationLookupStatus::NotFound, {}};
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.repository.commit_calls == 0,
               "unregistered subsystem ID is rejected");
    }
    {
        Fixture fixture;
        fixture.registration.result = {
            SubsystemRegistrationLookupStatus::Unavailable, {}};
        expect(process(fixture) == EventProcessorStatus::Error &&
                   fixture.repository.commit_calls == 0,
               "unavailable registration service does not authorize an update");
    }
    {
        Fixture fixture;
        fixture.registration.result = {
            SubsystemRegistrationLookupStatus::NotFound,
            {"station-1", "weather.temperature", "esp32.main",
             "registration-rev-7", SubsystemRegistrationState::Active}};
        expect(process(fixture) == EventProcessorStatus::Error &&
                   fixture.repository.commit_calls == 0,
               "contradictory NOT_FOUND registry response fails closed");
    }
    {
        Fixture fixture;
        fixture.registration.result.record.station_id = "station-2";
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.repository.commit_calls == 0,
               "registration for another station cannot be used");
    }
    {
        Fixture fixture;
        fixture.registration.result.record.subsystem_id = "lid.motor";
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.repository.commit_calls == 0,
               "registration must match the exact subsystem ID");
    }
    {
        Fixture fixture;
        fixture.registration.result.record.source = "another.controller";
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.repository.commit_calls == 0,
               "subsystem registration must bind the exact reporting source");
    }
    {
        Fixture fixture;
        fixture.registration.result.record.state =
            SubsystemRegistrationState::Disabled;
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.repository.commit_calls == 0,
               "disabled subsystem registration is rejected");
    }
    {
        Fixture fixture;
        fixture.registration.result.record.state =
            SubsystemRegistrationState::Revoked;
        expect(process(fixture) == EventProcessorStatus::Rejected &&
                   fixture.repository.commit_calls == 0,
               "revoked subsystem registration is rejected");
    }
    {
        Fixture fixture;
        fixture.registration.result.record.state =
            static_cast<SubsystemRegistrationState>(255U);
        expect(process(fixture) == EventProcessorStatus::Error &&
                   fixture.repository.commit_calls == 0,
               "unknown registry state fails closed");
    }
    {
        Fixture fixture;
        const Event event = make_event();
        const SourceCursor cursor = make_cursor();
        expect(process(fixture, event, EventCategory::Subsystem, cursor) ==
                   EventProcessorStatus::NoStateChange &&
                   fixture.registration.requested_station == "station-1" &&
                   fixture.registration.requested_subsystem ==
                       "weather.temperature" &&
                   fixture.repository.commit_calls == 1 &&
                   fixture.repository.committed_registration.registration_revision ==
                       "registration-rev-7" &&
                   fixture.repository.committed_report.status ==
                       ReportedSubsystemStatus::Degraded &&
                   fixture.repository.committed_event.event_id == event.event_id &&
                   fixture.repository.committed_cursor.last_sequence ==
                       cursor.last_sequence,
               "active exact registration and decoded report reach atomic commit");
    }
    {
        Fixture fixture;
        fixture.repository.commit_result =
            {SubsystemStatusCommitStatus::Committed};
        expect(process(fixture) == EventProcessorStatus::NoStateChange &&
                   fixture.repository.commit_calls == 1,
               "subsystem update is committed without changing global StationState");
    }
    {
        Fixture fixture;
        fixture.repository.commit_result =
            {SubsystemStatusCommitStatus::DuplicateEvent};
        expect(process(fixture) == EventProcessorStatus::Duplicate,
               "commit-time duplicate race is reported as duplicate");
    }
    {
        Fixture fixture;
        fixture.repository.commit_result =
            {SubsystemStatusCommitStatus::EventConflict};
        expect(process(fixture) == EventProcessorStatus::EventConflict,
               "commit-time event ID reuse is reported as conflict");
    }
    {
        Fixture fixture;
        fixture.repository.commit_result =
            {SubsystemStatusCommitStatus::RegistrationChanged};
        expect(process(fixture) == EventProcessorStatus::ConcurrentUpdate,
               "registration revision race blocks commit");
    }
    {
        Fixture fixture;
        fixture.repository.commit_result =
            {SubsystemStatusCommitStatus::SourceCursorChanged};
        expect(process(fixture) == EventProcessorStatus::ConcurrentUpdate,
               "source cursor race blocks commit");
    }
    {
        Fixture fixture;
        fixture.repository.commit_result =
            {SubsystemStatusCommitStatus::SubsystemNotRegistered};
        expect(process(fixture) == EventProcessorStatus::Rejected,
               "subsystem removal between lookup and commit blocks the update");
    }
    {
        Fixture fixture;
        fixture.repository.commit_result =
            {SubsystemStatusCommitStatus::Error};
        expect(process(fixture) == EventProcessorStatus::Error,
               "unconfirmed repository commit never reports success");
    }
    {
        Fixture fixture;
        fixture.repository.commit_result = {
            static_cast<SubsystemStatusCommitStatus>(255U)};
        expect(process(fixture) == EventProcessorStatus::Error,
               "unknown commit status fails closed");
    }
    {
        Fixture fixture;
        const DecodedSubsystemStatus report = make_report();
        expect(is_known_reported_subsystem_status(report.status) &&
                   !is_known_reported_subsystem_status(
                       static_cast<ReportedSubsystemStatus>(255U)),
               "reported subsystem status validator rejects unknown enum values");
    }
    {
        SubsystemRegistrationLookupResult malformed{
            SubsystemRegistrationLookupStatus::Found, {}};
        expect(!is_valid_subsystem_registration_lookup_result(malformed),
               "FOUND registry result must contain a complete registration");
    }

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
