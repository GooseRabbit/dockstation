#include <cstdlib>
#include <iostream>
#include <string_view>
#include "core/operation_event_processor.hpp"
using namespace dockstation::core;
namespace {
int checks=0, failures=0, f09_calls=0;
OperationUpdateResult f09_result{};
OperationUpdate last_update{};
SourceCursor last_cursor{};
OperationUpdateRepository* last_repo=nullptr;
void check(bool ok,std::string_view label){++checks;if(!ok){++failures;std::cout<<"FAIL "<<label<<'\n';}}
Event make_event(){Event e{};e.event_id="evt";e.event_type="OPERATION_STATUS_UPDATED";e.source="module";
 e.source_instance="boot";e.received_at="now";e.source_sequence=12;e.operation_id="op";
 e.correlation_id="corr";e.payload="opaque";e.quality_status="VALID";e.schema_version="1";return e;}
SourceCursor make_cursor(){return {"module",std::string_view{"boot"},11};}
class Decoder final:public OperationUpdateDecoderPort{
public: OperationUpdateDecodeResult result{OperationUpdateDecodeStatus::Decoded,OperationLifecycleStatus::Accepted};
 int calls=0;const Event* seen=nullptr;
 OperationUpdateDecodeResult decode_next_status(const Event&e) noexcept override{++calls;seen=&e;return result;}
};
class Repository final:public OperationUpdateRepository{
public:
 EventRepositoryLookupResult find_by_id(std::string_view) noexcept override{return {EventRepositoryLookupStatus::NotFound,{}};}
 OperationLookupResult find_operation(std::string_view) noexcept override{return {OperationLookupStatus::NotFound,{}};}
 OperationUpdateCommitStatus commit_operation_update(const OperationRecord&,OperationLifecycleStatus,const Event&,const SourceCursor&) noexcept override{return OperationUpdateCommitStatus::Error;}
};
EventProcessorStatus run(Event&e,Decoder&d,Repository&r,EventCategory c=EventCategory::Operation,const SourceCursor&cur=make_cursor()){
 OperationUpdateEventProcessor p(d,r);return p.process(e,c,cur);}
}
namespace dockstation::core {
OperationUpdateResult handle_operation_update(const OperationUpdate&u,const SourceCursor&c,OperationUpdateRepository&r) noexcept{
 ++f09_calls;last_update=u;last_cursor=c;last_repo=&r;return f09_result;}
}
int main(){
 {auto e=make_event();Decoder d;Repository r;f09_calls=0;f09_result={OperationUpdateResultStatus::Applied,OperationLifecycleStatus::Accepted,2};
  auto cur=make_cursor();check(run(e,d,r,EventCategory::Operation,cur)==EventProcessorStatus::NoStateChange,"Applied mapping");
  check(d.calls==1&&f09_calls==1,"single decode and F09 call");check(d.seen==&e&&last_update.event.payload==e.payload,"original opaque event forwarded");
  check(last_cursor.source==cur.source&&last_cursor.source_instance==cur.source_instance&&last_cursor.last_sequence==cur.last_sequence,"cursor forwarded unchanged");
  check(last_update.next_status==OperationLifecycleStatus::Accepted&&last_repo==&r,"typed status and repository forwarded");}
 {auto e=make_event();Decoder d;Repository r;f09_calls=0;
  check(run(e,d,r,EventCategory::Subsystem)==EventProcessorStatus::Unsupported,"wrong category");
  e=make_event();e.event_type="OPERATION_TIMEOUT";check(run(e,d,r)==EventProcessorStatus::Unsupported,"wrong event type");
  check(d.calls==0&&f09_calls==0,"unsupported events stop before decoder and F09");}
 for(auto q:{"STALE","INVALID","UNAVAILABLE","UNKNOWN","ERROR","valid",""}){
  auto e=make_event();e.quality_status=q;Decoder d;Repository r;f09_calls=0;
  check(run(e,d,r)==EventProcessorStatus::Rejected,"quality gate rejects non-exact VALID");
  check(d.calls==0&&f09_calls==0,"quality gate blocks downstream");}
 {auto e=make_event();Decoder d;Repository r;d.result.status=OperationUpdateDecodeStatus::UnsupportedPayload;f09_calls=0;
  check(run(e,d,r)==EventProcessorStatus::Rejected&&f09_calls==0,"unsupported payload rejected");}
 {auto e=make_event();Decoder d;Repository r;d.result.status=OperationUpdateDecodeStatus::Error;f09_calls=0;
  check(run(e,d,r)==EventProcessorStatus::Error&&f09_calls==0,"decoder error fails closed");}
 {auto e=make_event();Decoder d;Repository r;d.result.next_status.reset();f09_calls=0;
  check(run(e,d,r)==EventProcessorStatus::Error&&f09_calls==0,"decoded without status fails closed");}
 {auto e=make_event();Decoder d;Repository r;d.result.status=static_cast<OperationUpdateDecodeStatus>(255);f09_calls=0;
  check(run(e,d,r)==EventProcessorStatus::Error&&f09_calls==0,"unknown decoder enum fails closed");}
 struct Map{OperationUpdateResultStatus in;EventProcessorStatus out;};
 const Map maps[]={
 {OperationUpdateResultStatus::Applied,EventProcessorStatus::NoStateChange},
 {OperationUpdateResultStatus::Duplicate,EventProcessorStatus::Duplicate},
 {OperationUpdateResultStatus::EventConflict,EventProcessorStatus::EventConflict},
 {OperationUpdateResultStatus::InvalidEvent,EventProcessorStatus::Rejected},
 {OperationUpdateResultStatus::EventNotReliable,EventProcessorStatus::Rejected},
 {OperationUpdateResultStatus::WrongEventType,EventProcessorStatus::Rejected},
 {OperationUpdateResultStatus::InvalidStatus,EventProcessorStatus::Rejected},
 {OperationUpdateResultStatus::StaleEvent,EventProcessorStatus::Error},
 {OperationUpdateResultStatus::EventOrderUnknown,EventProcessorStatus::Error},
 {OperationUpdateResultStatus::OperationNotFound,EventProcessorStatus::Rejected},
 {OperationUpdateResultStatus::CorrelationMismatch,EventProcessorStatus::Rejected},
 {OperationUpdateResultStatus::TransitionNotAllowed,EventProcessorStatus::Rejected},
 {OperationUpdateResultStatus::ConcurrentOperationUpdate,EventProcessorStatus::ConcurrentUpdate},
 {OperationUpdateResultStatus::ConcurrentSourceUpdate,EventProcessorStatus::ConcurrentUpdate},
 {OperationUpdateResultStatus::RepositoryError,EventProcessorStatus::Error},
 {static_cast<OperationUpdateResultStatus>(255),EventProcessorStatus::Error}};
 for(const auto&m:maps){auto e=make_event();Decoder d;Repository r;f09_calls=0;f09_result={m.in,OperationLifecycleStatus::Accepted,2};
  check(run(e,d,r)==m.out,"F09 result mapping");check(f09_calls==1,"F09 called once after successful decode");}
 std::cout<<"Checks: "<<checks<<"\nFailures: "<<failures<<'\n';return failures==0?EXIT_SUCCESS:EXIT_FAILURE;
}
