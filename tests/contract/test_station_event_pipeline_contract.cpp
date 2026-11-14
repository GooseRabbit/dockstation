#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include "core/station_event_pipeline.hpp"

using namespace dockstation::core;
namespace {
int checks=0, failures=0;
void expect(bool ok,std::string_view msg){++checks;if(!ok){++failures;std::cout<<"FAIL: "<<msg<<'\n';}}
Event make_event(std::string_view id,std::string_view type,std::uint64_t seq){
 Event e{};e.event_id=id;e.event_type=type;e.source="station-events";e.source_instance="boot-1";
 e.received_at="receiver-time";e.source_sequence=seq;e.payload="opaque-payload";
 e.quality_status="VALID";e.schema_version="1";return e;
}
SourceSessionEvidence evidence(){return {"station-events","boot-1","authenticated-peer","session-proof"};}
class Events final:public EventRepository{
public: EventRepositoryLookupStatus status{EventRepositoryLookupStatus::NotFound};Event stored{};int calls{0};
 EventRepositoryLookupResult find_by_id(std::string_view id)noexcept override{
  ++calls;if(status==EventRepositoryLookupStatus::Found&&stored.event_id==id)return{status,stored};
  if(status==EventRepositoryLookupStatus::Found)return{EventRepositoryLookupStatus::NotFound,{}};
  return{status,{}};
 }
};
class Verifier final:public SourceSessionVerifier{
public:SourceSessionVerificationStatus status{SourceSessionVerificationStatus::Verified};int calls{0};
 SourceSessionVerificationResult verify_source_session(const SourceSessionEvidence&e)noexcept override{
  ++calls;if(status!=SourceSessionVerificationStatus::Verified)return{status,{}};
  return{status,{e.source,e.source_instance,"registration-revision-1"}};
 }
};
class CursorReader final:public SourceCursorReader{
public:SourceCursorReadStatus status{SourceCursorReadStatus::Found};std::string source{"station-events"};
 std::string instance{"boot-1"};std::optional<std::uint64_t> sequence{0U};int calls{0};
 SourceCursorReadResult read_source_cursor(std::string_view requested)noexcept override{
  ++calls;if(status!=SourceCursorReadStatus::Found)return{status,{}};
  if(requested!=source)return{SourceCursorReadStatus::NotFound,{}};
  return{status,{source,std::string_view{instance},sequence}};
 }
};
class Sessions final:public SourceSessionRepository{
public:SourceSessionConfirmationStatus forced{SourceSessionConfirmationStatus::Confirmed};int calls{0};
 SourceSessionConfirmationResult confirm_source_session(const SourceCursorReadResult&expected,
  const VerifiedSourceSession&verified)noexcept override{
  ++calls;if(forced!=SourceSessionConfirmationStatus::Confirmed)return{forced,{}};
  if(expected.status==SourceCursorReadStatus::Found)return{SourceSessionConfirmationStatus::AlreadyCurrent,expected.cursor};
  return{SourceSessionConfirmationStatus::Confirmed,{verified.source,verified.source_instance,std::nullopt}};
 }
};
class Processor final:public EventProcessorPort{
public:EventProcessorStatus result{EventProcessorStatus::Processed};int calls{0};
 const Event* last{nullptr};std::string_view type{};EventCategory category{EventCategory::Unknown};
 EventProcessorStatus process(const Event&e,EventCategory c,const SourceCursor&)noexcept override{
  ++calls;last=&e;type=e.event_type;category=c;return result;
 }
};
struct Fixture{
 Events events;Processor global,operation,timeout;Verifier verifier;CursorReader reader;Sessions sessions;
 StationEventPipeline pipeline;
 Fixture():pipeline(events,global,operation,timeout,verifier,reader,sessions){
  operation.result=EventProcessorStatus::NoStateChange;timeout.result=EventProcessorStatus::NoStateChange;
 }
 StationEventPipelineResult handle(const Event&e){return pipeline.handle(e,{true,true,false},evidence());}
 void seq(std::uint64_t n){reader.status=SourceCursorReadStatus::Found;reader.sequence=n;}
};
}
int main(){
 Fixture f;expect(f.pipeline.is_configured(),"pipeline has valid built-in routes");
 const struct{const char*id;const char*type;EventHandlingStatus expected;int g,o,t;} cases[]={
  {"evt-1","STARTUP_REQUESTED",EventHandlingStatus::Processed,1,0,0},
  {"evt-2","INITIALIZATION_COMPLETED",EventHandlingStatus::Processed,2,0,0},
  {"evt-3","SELF_TEST_COMPLETED",EventHandlingStatus::Processed,3,0,0},
  {"evt-4","OPERATION_STATUS_UPDATED",EventHandlingStatus::NoStateChange,3,1,0},
  {"evt-5","OPERATION_TIMEOUT",EventHandlingStatus::NoStateChange,3,1,1}};
 std::uint64_t n=1;
 for(const auto&c:cases){f.seq(n-1);auto r=f.handle(make_event(c.id,c.type,n));
  expect(r.session_status==SourceSessionCoordinatorStatus::AlreadyCurrent,"authenticated current session confirmed");
  expect(r.event_result&&r.event_result->status==c.expected,"event reaches expected F13 result");
  expect(f.global.calls==c.g&&f.operation.calls==c.o&&f.timeout.calls==c.t,"exactly one selected profile called");++n;}
 expect(f.global.last&&f.global.type=="SELF_TEST_COMPLETED"&&f.global.category==EventCategory::Subsystem,"startup events route to global profile");
 expect(f.operation.last&&f.operation.type=="OPERATION_STATUS_UPDATED"&&f.operation.category==EventCategory::Operation,"operation event routes correctly");
 expect(f.timeout.last&&f.timeout.type=="OPERATION_TIMEOUT"&&f.timeout.category==EventCategory::Timer,"timeout event routes correctly");
 // F01: session must pass first, then invalid envelope is stopped before event lookup/profile.
 {f.seq(5);Event e=make_event("invalid","OPERATION_TIMEOUT",6);e.event_id="   ";int before=f.events.calls,pc=f.timeout.calls;
  auto r=f.handle(e);expect(r.event_result&&r.event_result->status==EventHandlingStatus::InvalidEvent,"F01 rejects invalid envelope");
  expect(f.events.calls==before&&f.timeout.calls==pc,"F01 prevents event lookup/profile");}
 // F02 unknown event.
 {f.seq(5);int before=f.events.calls;auto r=f.handle(make_event("unknown","SOMETHING_NEW",6));
  expect(r.event_result&&r.event_result->status==EventHandlingStatus::UnknownEventType,"F02 rejects unknown type");
  expect(f.events.calls==before,"F02 precedes duplicate lookup");}
 // F03 duplicate and repository error.
 {f.seq(5);auto e=make_event("duplicate","OPERATION_TIMEOUT",6);f.events.status=EventRepositoryLookupStatus::Found;f.events.stored=e;
  int before=f.timeout.calls;auto r=f.handle(e);expect(r.event_result&&r.event_result->status==EventHandlingStatus::Duplicate,"F03 detects duplicate");
  expect(f.timeout.calls==before,"duplicate is not routed");}
 {f.seq(5);f.events.status=EventRepositoryLookupStatus::Error;int before=f.timeout.calls;
  auto r=f.handle(make_event("repo-error","OPERATION_TIMEOUT",6));expect(r.event_result&&r.event_result->status==EventHandlingStatus::RepositoryError,"F03 fails closed on repository error");
  expect(f.timeout.calls==before,"repository error prevents routing");}
 f.events.status=EventRepositoryLookupStatus::NotFound;
 // F04 stale and repeated sequence.
 {f.seq(5);int before=f.timeout.calls;auto r=f.handle(make_event("stale","OPERATION_TIMEOUT",4));
  expect(r.event_result&&r.event_result->status==EventHandlingStatus::StaleEvent,"F04 rejects regressed sequence");
  expect(f.timeout.calls==before,"stale event is not routed");}
 {f.seq(5);int before=f.timeout.calls;auto r=f.handle(make_event("repeat","OPERATION_TIMEOUT",5));
  expect(r.event_result&&r.event_result->status==EventHandlingStatus::EventOrderUnknown,"F04 rejects repeated sequence");
  expect(f.timeout.calls==before,"unordered event is not routed");}
 // Known catalogue type without a profile.
 {f.seq(5);auto r=f.handle(make_event("unsupported","SUBSYSTEM_STATUS_UPDATED",6));
  expect(r.event_result&&r.event_result->status==EventHandlingStatus::Unsupported,"known unsupported profile stays unsupported");}
 // Profile returning Unsupported is an integration error.
 {f.seq(5);f.timeout.result=EventProcessorStatus::Unsupported;auto r=f.handle(make_event("mismatch","OPERATION_TIMEOUT",6));
  expect(r.event_result&&r.event_result->status==EventHandlingStatus::ProcessingError,"profile mismatch maps to processing error");}
 // Session failures never enter F13.
 {f.verifier.status=SourceSessionVerificationStatus::Rejected;int ec=f.events.calls,gc=f.global.calls;
  auto r=f.handle(make_event("rejected","STARTUP_REQUESTED",7));
  expect(r.session_status==SourceSessionCoordinatorStatus::Rejected&&!r.event_result,"rejected session has no F13 result");
  expect(f.events.calls==ec&&f.global.calls==gc,"rejected session blocks event lookup and profile");}
 // Event source instance cannot be substituted for authenticated channel identity.
 {f.verifier.status=SourceSessionVerificationStatus::Verified;int vc=f.verifier.calls,ec=f.events.calls;
  auto e=make_event("old-boot","STARTUP_REQUESTED",7);e.source_instance="boot-old";auto r=f.handle(e);
  expect(r.session_status==SourceSessionCoordinatorStatus::InvalidEvidence&&!r.event_result,"old boot instance is rejected");
  expect(f.verifier.calls==vc&&f.events.calls==ec,"instance mismatch is rejected before verifier and F13");}
 // Cursor-read and registration-race errors are fail-closed.
 {f.reader.status=SourceCursorReadStatus::Error;int ec=f.events.calls;auto r=f.handle(make_event("cursor-error","STARTUP_REQUESTED",7));
  expect(r.session_status==SourceSessionCoordinatorStatus::CursorReadError&&!r.event_result,"cursor read error blocks F13");
  expect(f.events.calls==ec,"cursor read error causes no event lookup");}
 {f.reader.status=SourceCursorReadStatus::Found;f.sessions.forced=SourceSessionConfirmationStatus::RegistrationChanged;int ec=f.events.calls;
  auto r=f.handle(make_event("registration-race","STARTUP_REQUESTED",7));
  expect(r.session_status==SourceSessionCoordinatorStatus::RegistrationChanged&&!r.event_result,"registration race preserves exact status");
  expect(f.events.calls==ec,"registration race blocks event lookup");}
 std::cout<<"Checks: "<<checks<<'\n'<<"Failures: "<<failures<<'\n';
 return failures==0?EXIT_SUCCESS:EXIT_FAILURE;
}
