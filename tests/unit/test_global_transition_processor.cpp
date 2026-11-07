#include <cstdlib>
#include <iostream>
#include <string_view>
#include "global_transition_processor.hpp"
using namespace dockstation::core;
namespace {
int failures=0, checks=0;
void expect(bool ok,std::string_view name){++checks;std::cout<<(ok?"PASS":"FAIL")<<" - "<<name<<'\n';if(!ok)++failures;}
Event make_event(std::string_view type,std::string_view quality="VALID"){
 Event e{};e.event_id="event-1";e.event_type=type;e.source="startup-controller";
 e.source_instance="boot-1";e.source_sequence=10;e.received_at="receiver-time";
 e.quality_status=quality;e.schema_version="1";return e;
}
SourceCursor cursor(){return {"startup-controller","boot-1",9};}
struct FakeSnapshotReader final:StationStateSnapshotReader{
 SnapshotReadPortResult result{SnapshotReadPortStatus::Found,StationStateSnapshot{StationState::PoweredOff,0,{}}};
 int calls{0};SnapshotReadPortResult read_snapshot() noexcept override{++calls;return result;}
};
struct FakeContextProvider final:TransitionContextProvider{
 TransitionContextReadResult result{TransitionContextReadStatus::Available,{}};
 int calls{0};Event seen{};TransitionContextReadResult read_transition_context(const Event&e) noexcept override{++calls;seen=e;return result;}
};
struct FakeIdProvider final:TransitionIdProvider{
 bool available{true};std::string_view value{"transition-1"};int calls{0};
 bool next_transition_id(std::string_view&out) noexcept override{++calls;out=value;return available;}
};
struct FakeRepository final:StationStateRepository{
 StationStateRepositoryStatus status{StationStateRepositoryStatus::Committed};int calls{0};
 TransitionPlan plan{};TransitionEvidence evidence{};StationStateSnapshot next{};SourceCursor cursor{};
 StationStateRepositoryStatus commit_transition(const TransitionPlan&p,const TransitionEvidence&e,
 const StationStateSnapshot&n,const SourceCursor&c) noexcept override{
 ++calls;plan=p;evidence=e;next=n;cursor=c;return status;
 }
};
struct FakePublisher final:StateChangedPublisher{
 StateChangedPublishStatus status{StateChangedPublishStatus::Published};int calls{0};
 StateChangedPublishStatus publish(const StateChanged&) noexcept override{++calls;return status;}
};
struct Fixture{
 FakeSnapshotReader snapshot;FakeContextProvider context;FakeIdProvider ids;FakeRepository repository;FakePublisher publisher;
 GlobalTransitionProcessor processor{snapshot,context,ids,repository,publisher};
};
}
int main(){
 // Три точные стартовые пары.
 {
  Fixture f;auto r=f.processor.process(make_event("STARTUP_REQUESTED"),EventCategory::Command,cursor());
  expect(r==EventProcessorStatus::Processed,"STARTUP_REQUESTED enters STARTUP");
  expect(f.context.calls==0,"startup does not request unnecessary context");
  expect(f.ids.calls==1&&f.repository.calls==1,"allowed startup obtains ID and commits once");
  expect(f.repository.next.state==StationState::Startup&&f.repository.next.revision==1,"startup snapshot revision increments");
 }
 {
  Fixture f;f.snapshot.result.snapshot=StationStateSnapshot{StationState::Startup,4,"old"};
  f.context.result.context.configuration_validated=true;auto e=make_event("INITIALIZATION_COMPLETED");
  auto r=f.processor.process(e,EventCategory::Subsystem,cursor());
  expect(r==EventProcessorStatus::Processed,"INITIALIZATION_COMPLETED enters SELF_TEST");
  expect(f.context.calls==1&&f.context.seen.event_id==e.event_id,"context provider receives original event");
  expect(f.repository.next.state==StationState::SelfTest&&f.repository.next.revision==5,"initialization snapshot revision increments");
 }
 {
  Fixture f;f.snapshot.result.snapshot=StationStateSnapshot{StationState::SelfTest,7,"old"};
  f.context.result.context={true,true,true,true,RecoveryStatus::NotRequired};
  auto r=f.processor.process(make_event("SELF_TEST_COMPLETED"),EventCategory::Subsystem,cursor());
  expect(r==EventProcessorStatus::Processed,"SELF_TEST_COMPLETED enters WAITING with full evidence");
  expect(f.repository.next.state==StationState::Waiting,"self-test commit target is WAITING");
 }
 // Unsupported event/category and bad quality.
 {
  Fixture f;auto r=f.processor.process(make_event("COMMAND_REQUESTED"),EventCategory::Command,cursor());
  expect(r==EventProcessorStatus::Unsupported,"unimplemented command remains unsupported");
  expect(f.snapshot.calls==0&&f.ids.calls==0&&f.repository.calls==0,"unsupported event has no downstream effects");
 }
 {
  Fixture f;expect(f.processor.process(make_event("STARTUP_REQUESTED"),EventCategory::Subsystem,cursor())==EventProcessorStatus::Unsupported,
   "event/category mismatch is unsupported");
 }
 {
  Fixture f;expect(f.processor.process(make_event("STARTUP_REQUESTED","UNKNOWN"),EventCategory::Command,cursor())==EventProcessorStatus::Rejected,
   "non-VALID event is rejected");
  expect(f.snapshot.calls==0&&f.repository.calls==0,"unreliable event cannot reach commit");
 }
 // Snapshot absence/error fails closed.
 {
  Fixture f;f.snapshot.result.status=SnapshotReadPortStatus::NotFound;
  expect(f.processor.process(make_event("STARTUP_REQUESTED"),EventCategory::Command,cursor())==EventProcessorStatus::Error,
   "missing snapshot is not replaced with POWERED_OFF");
  expect(f.ids.calls==0&&f.repository.calls==0,"missing snapshot blocks ID and commit");
 }
 {
  Fixture f;f.snapshot.result.status=SnapshotReadPortStatus::Error;
  expect(f.processor.process(make_event("STARTUP_REQUESTED"),EventCategory::Command,cursor())==EventProcessorStatus::Error,
   "snapshot read error fails closed");
 }
 // Context unavailability drops partial positives; error/unknown enums fail closed.
 {
  Fixture f;f.snapshot.result.snapshot=StationStateSnapshot{StationState::Startup,2,"old"};
  f.context.result.status=TransitionContextReadStatus::Unavailable;f.context.result.context.configuration_validated=true;
  expect(f.processor.process(make_event("INITIALIZATION_COMPLETED"),EventCategory::Subsystem,cursor())==EventProcessorStatus::Rejected,
   "unavailable context discards partial positive data");
  expect(f.ids.calls==0&&f.repository.calls==0,"missing context blocks ID and commit");
 }
 {
  Fixture f;f.snapshot.result.snapshot=StationStateSnapshot{StationState::Startup,2,"old"};
  f.context.result.status=TransitionContextReadStatus::Error;
  expect(f.processor.process(make_event("INITIALIZATION_COMPLETED"),EventCategory::Subsystem,cursor())==EventProcessorStatus::Error,
   "context provider error fails closed");
 }
 {
  Fixture f;f.snapshot.result.snapshot=StationStateSnapshot{StationState::Startup,2,"old"};
  f.context.result.status=static_cast<TransitionContextReadStatus>(255);
  expect(f.processor.process(make_event("INITIALIZATION_COMPLETED"),EventCategory::Subsystem,cursor())==EventProcessorStatus::Error,
   "unknown context status fails closed");
 }
 {
  Fixture f;f.snapshot.result.snapshot=StationStateSnapshot{StationState::SelfTest,2,"old"};
  f.context.result.context={true,true,true,true,RecoveryStatus::Unknown};
  expect(f.processor.process(make_event("SELF_TEST_COMPLETED"),EventCategory::Subsystem,cursor())==EventProcessorStatus::Rejected,
   "UNKNOWN recovery status cannot permit WAITING");
 }
 // Transition ID is requested only after F05 approves the transition.
 {
  Fixture f;f.snapshot.result.snapshot=StationStateSnapshot{StationState::Waiting,3,"old"};
  expect(f.processor.process(make_event("STARTUP_REQUESTED"),EventCategory::Command,cursor())==EventProcessorStatus::Rejected,
   "F05 rejects an unapproved transition");
  expect(f.ids.calls==0,"rejected transition does not request an ID");
 }
 {
  Fixture f;f.ids.available=false;
  expect(f.processor.process(make_event("STARTUP_REQUESTED"),EventCategory::Command,cursor())==EventProcessorStatus::Error,
   "unavailable ID provider fails closed");
  expect(f.repository.calls==0,"ID provider failure prevents commit");
 }
 {
  Fixture f;f.ids.value=" \t\r\n ";
  expect(f.processor.process(make_event("STARTUP_REQUESTED"),EventCategory::Command,cursor())==EventProcessorStatus::Error,
   "whitespace-only transition ID is rejected");
 }
 // F10 outcome mapping and unchanged source cursor.
 const StationStateRepositoryStatus repo_statuses[]={StationStateRepositoryStatus::DuplicateEvent,StationStateRepositoryStatus::EventConflict,
 StationStateRepositoryStatus::TransitionIdConflict,StationStateRepositoryStatus::SnapshotChanged,
 StationStateRepositoryStatus::SourceCursorChanged,StationStateRepositoryStatus::Error};
 const EventProcessorStatus mapped[]={EventProcessorStatus::Duplicate,EventProcessorStatus::EventConflict,EventProcessorStatus::Error,
 EventProcessorStatus::ConcurrentUpdate,EventProcessorStatus::ConcurrentUpdate,EventProcessorStatus::Error};
 for(std::size_t i=0;i<sizeof(repo_statuses)/sizeof(repo_statuses[0]);++i){
  Fixture f;f.repository.status=repo_statuses[i];const SourceCursor expected=cursor();
  auto r=f.processor.process(make_event("STARTUP_REQUESTED"),EventCategory::Command,expected);
  expect(r==mapped[i],"F10 repository result maps to a non-success status");
  expect(f.repository.calls==1&&f.repository.cursor.source==expected.source&&
         f.repository.cursor.last_sequence==expected.last_sequence,"original source cursor is passed unchanged");
  expect(f.publisher.calls==0,"publisher is not called without repository commit");
 }
 {
  Fixture f;f.publisher.status=StateChangedPublishStatus::Unknown;
  expect(f.processor.process(make_event("STARTUP_REQUESTED"),EventCategory::Command,cursor())==EventProcessorStatus::CommittedNotificationNotConfirmed,
   "committed state with unknown publication has a distinct result");
  expect(f.repository.calls==1&&f.publisher.calls==1,"publication uncertainty occurs after commit only");
 }
 {
  Fixture f;f.publisher.status=StateChangedPublishStatus::Rejected;
  expect(f.processor.process(make_event("STARTUP_REQUESTED"),EventCategory::Command,cursor())==EventProcessorStatus::CommittedNotificationNotConfirmed,
   "rejected publication is not confused with failed commit");
 }
 std::cout<<"\nChecks: "<<checks<<"\nFailures: "<<failures<<'\n';
 return failures==0?EXIT_SUCCESS:EXIT_FAILURE;
}
