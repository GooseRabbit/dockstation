#include "core/station_event_pipeline.hpp"
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {
using namespace dockstation::core;
int failures = 0;
int checks = 0;
void expect(bool ok, std::string_view message) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

class Verifier final : public SourceSessionVerifier {
public:
    SourceSessionVerificationStatus status{SourceSessionVerificationStatus::Verified};
    int calls{0};
    SourceSessionVerificationResult verify_source_session(
        const SourceSessionEvidence& e) noexcept override {
        ++calls;
        if (status != SourceSessionVerificationStatus::Verified) return {status, {}};
        return {status, {e.source, e.source_instance, "revision-1"}};
    }
};
class CursorReader final : public SourceCursorReader {
public:
    SourceCursorReadStatus status{SourceCursorReadStatus::NotFound};
    std::string source{"sensor/esp32"};
    std::string instance{"boot-current"};
    std::optional<std::uint64_t> sequence{};
    int calls{0};
    SourceCursorReadResult read_source_cursor(std::string_view key) noexcept override {
        ++calls;
        if (status != SourceCursorReadStatus::Found) return {status, {}};
        if (key != source) return {SourceCursorReadStatus::NotFound, {}};
        return {status, {source, std::string_view{instance}, sequence}};
    }
};
class SessionRepository final : public SourceSessionRepository {
public:
    SourceSessionConfirmationStatus forced{SourceSessionConfirmationStatus::Confirmed};
    int calls{0};
    SourceSessionConfirmationResult confirm_source_session(
        const SourceCursorReadResult& expected,
        const VerifiedSourceSession& verified) noexcept override {
        ++calls;
        if (forced != SourceSessionConfirmationStatus::Confirmed) return {forced, {}};
        if (expected.status == SourceCursorReadStatus::Found &&
            expected.cursor.source_instance ==
                std::optional<std::string_view>{verified.source_instance}) {
            return {SourceSessionConfirmationStatus::AlreadyCurrent, expected.cursor};
        }
        return {SourceSessionConfirmationStatus::Confirmed,
                {verified.source, verified.source_instance, std::nullopt}};
    }
};
class EventRepositoryFake final : public EventRepository {
public:
    int calls{0};
    EventRepositoryLookupResult find_by_id(std::string_view) noexcept override {
        ++calls;
        return {EventRepositoryLookupStatus::NotFound, {}};
    }
};
class Processor final : public EventProcessorPort {
public:
    int calls{0};
    EventProcessorStatus process(const Event&, EventCategory,
                                 const SourceCursor&) noexcept override {
        ++calls; return EventProcessorStatus::Processed;
    }
};
Event event_value() {
    Event e{};
    e.event_id="evt-1"; e.event_type="STARTUP_REQUESTED";
    e.source="sensor/esp32"; e.source_instance="boot-current";
    e.received_at="2026-11-14T10:00:00Z"; e.source_sequence=1U;
    e.payload="{}"; e.quality_status="VALID"; e.schema_version="1";
    return e;
}
SourceSessionEvidence evidence_value() {
    return {"sensor/esp32","boot-current","authenticated-peer","proof"};
}
void rejected_session_blocks_f13() {
    EventRepositoryFake events; Processor global, operation, timeout;
    Verifier verifier; verifier.status=SourceSessionVerificationStatus::Rejected;
    CursorReader reader; SessionRepository sessions;
    StationEventPipeline pipeline{events,global,operation,timeout,verifier,reader,sessions};
    const auto r=pipeline.handle(event_value(),{true,true,false},evidence_value());
    expect(r.session_status==SourceSessionCoordinatorStatus::Rejected,"rejection status preserved");
    expect(!r.event_result,"no F13 result after rejection");
    expect(events.calls==0 && global.calls==0 && reader.calls==0 && sessions.calls==0,
           "rejected session never reaches F13 or cursor repository");
}
void confirmed_session_reaches_f13() {
    EventRepositoryFake events; Processor global, operation, timeout;
    Verifier verifier; CursorReader reader; SessionRepository sessions;
    StationEventPipeline pipeline{events,global,operation,timeout,verifier,reader,sessions};
    const auto r=pipeline.handle(event_value(),{true,true,false},evidence_value());
    expect(r.session_status==SourceSessionCoordinatorStatus::Confirmed,"session confirmed");
    expect(r.event_result && r.event_result->status==EventHandlingStatus::Processed,
           "confirmed event completes F13");
    expect(events.calls==1 && global.calls==1 && operation.calls==0 && timeout.calls==0,
           "exact profile route invoked once");
}
void old_instance_is_blocked() {
    EventRepositoryFake events; Processor global, operation, timeout;
    Verifier verifier; CursorReader reader; SessionRepository sessions;
    StationEventPipeline pipeline{events,global,operation,timeout,verifier,reader,sessions};
    Event old=event_value(); old.source_instance="boot-old";
    const auto r=pipeline.handle(old,{true,true,false},evidence_value());
    expect(r.session_status==SourceSessionCoordinatorStatus::InvalidEvidence && !r.event_result,
           "old instance is rejected before F13");
    expect(verifier.calls==0 && reader.calls==0 && sessions.calls==0 &&
           events.calls==0 && global.calls==0,"old instance reaches no downstream port");
}
void cursor_and_registration_errors_block() {
    {
        EventRepositoryFake events; Processor global, operation, timeout;
        Verifier verifier; CursorReader reader; reader.status=SourceCursorReadStatus::Error;
        SessionRepository sessions;
        StationEventPipeline pipeline{events,global,operation,timeout,verifier,reader,sessions};
        const auto r=pipeline.handle(event_value(),{true,true,false},evidence_value());
        expect(r.session_status==SourceSessionCoordinatorStatus::CursorReadError &&
               !r.event_result && events.calls==0 && global.calls==0,"cursor read error blocks F13");
    }
    {
        EventRepositoryFake events; Processor global, operation, timeout;
        Verifier verifier; CursorReader reader; SessionRepository sessions;
        sessions.forced=SourceSessionConfirmationStatus::RegistrationChanged;
        StationEventPipeline pipeline{events,global,operation,timeout,verifier,reader,sessions};
        const auto r=pipeline.handle(event_value(),{true,true,false},evidence_value());
        expect(r.session_status==SourceSessionCoordinatorStatus::RegistrationChanged &&
               !r.event_result && events.calls==0 && global.calls==0,
               "registration race blocks F13 and preserves status");
    }
}
}
int main() {
    rejected_session_blocks_f13();
    confirmed_session_reaches_f13();
    old_instance_is_blocked();
    cursor_and_registration_errors_block();
    if (failures) { std::cerr<<failures<<" of "<<checks<<" checks failed.\n"; return 1; }
    std::cout<<"All "<<checks<<" StationEventPipeline integration checks passed.\n";
    return 0;
}
