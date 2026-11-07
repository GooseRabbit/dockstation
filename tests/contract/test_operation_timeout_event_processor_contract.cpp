#include <cstdlib>
#include <iostream>
#include <string_view>
#include "core/operation_timeout_event_processor.hpp"
using namespace dockstation::core;
namespace {
int checks=0,failures=0,calls=0; OperationTimeoutResult f11_result{};
OperationTimeout last_timeout{}; SourceCursor last_cursor{}; OperationUpdateRepository* last_repo=nullptr;
void check(bool b,std::string_view s){++checks;if(!b){++failures;std::cout<<"FAIL "<<s<<'\n';}}
Event event(){Event e{};e.event_id="timer-1";e.event_type="OPERATION_TIMEOUT";e.source="timer";
 e.source_instance="boot";e.received_at="now";e.source_sequence=5;e.operation_id="op";
 e.correlation_id="corr";e.payload="opaque";e.quality_status="VALID";e.schema_version="1";return e;}
SourceCursor cursor(){return {"timer",std::string_view{"boot"},4};}
class Decoder final:public OperationTimeoutDecoderPort{
public: OperationTimeoutDecodeResult result{OperationTimeoutDecodeStatus::Decoded,
 OperationTimeoutType::BeforeDispatch,DispatchCertainty::NotDispatched};int count=0;const Event* seen=nullptr;
 OperationTimeoutDecodeResult decode_timeout(const Event&e) noexcept override{++count;seen=&e;return result;}
};
class Repo final:public OperationUpdateRepository{
public: EventRepositoryLookupResult find_by_id(std::string_view) noexcept override{return {EventRepositoryLookupStatus::NotFound,{}};}
 OperationLookupResult find_operation(std::string_view) noexcept override{return {OperationLookupStatus::NotFound,{}};}
 OperationUpdateCommitStatus commit_operation_update(const OperationRecord&,OperationLifecycleStatus,const Event&,const SourceCursor&) noexcept override{return OperationUpdateCommitStatus::Error;}
};
EventProcessorStatus run(Event&e,Decoder&d,Repo&r,EventCategory cat=EventCategory::Timer,const SourceCursor&cur=cursor()){
 OperationTimeoutEventProcessor p(d,r);return p.process(e,cat,cur);}
}
namespace dockstation::core {
OperationTimeoutResult handle_operation_timeout(const OperationTimeout&v,const SourceCursor&c,OperationUpdateRepository&r) noexcept{
 ++calls;last_timeout=v;last_cursor=c;last_repo=&r;return f11_result;}
}
int main(){
 {auto e=event();Decoder d;Repo r;calls=0;f11_result={OperationTimeoutResultStatus::Applied,OperationLifecycleStatus::Failed,2};
 auto cur=cursor();check(run(e,d,r,EventCategory::Timer,cur)==EventProcessorStatus::NoStateChange,"Applied maps NoStateChange");
 check(d.count==1&&calls==1,"valid event decoded and passed to F11 once");
 check(d.seen==&e&&last_timeout.event.payload==e.payload,"original event forwarded unchanged");
 check(last_cursor.source==cur.source&&last_cursor.source_instance==cur.source_instance&&last_cursor.last_sequence==cur.last_sequence,"source cursor forwarded unchanged");
 check(last_repo==&r&&last_timeout.timeout_type==OperationTimeoutType::BeforeDispatch&&last_timeout.dispatch_certainty==DispatchCertainty::NotDispatched,"typed timeout evidence and repository forwarded");}
 {auto e=event();Decoder d;Repo r;calls=0;check(run(e,d,r,EventCategory::Operation)==EventProcessorStatus::Unsupported,"wrong category unsupported");
 e=event();e.event_type="OPERATION_STATUS_UPDATED";check(run(e,d,r)==EventProcessorStatus::Unsupported,"wrong event type unsupported");
 check(d.count==0&&calls==0,"unsupported event has no downstream effects");}
 for(auto q:{"STALE","INVALID","UNAVAILABLE","UNKNOWN","ERROR","valid",""}){
 auto e=event();e.quality_status=q;Decoder d;Repo r;calls=0;
 check(run(e,d,r)==EventProcessorStatus::Rejected,"non-VALID quality rejected");check(d.count==0&&calls==0,"quality gate blocks decoder and F11");}
 {auto e=event();Decoder d;Repo r;d.result.status=OperationTimeoutDecodeStatus::UnsupportedPayload;calls=0;
 check(run(e,d,r)==EventProcessorStatus::Rejected&&calls==0,"unsupported payload rejected");}
 {auto e=event();Decoder d;Repo r;d.result.status=OperationTimeoutDecodeStatus::Error;calls=0;
 check(run(e,d,r)==EventProcessorStatus::Error&&calls==0,"decoder error fails closed");}
 {auto e=event();Decoder d;Repo r;d.result.status=static_cast<OperationTimeoutDecodeStatus>(255);calls=0;
 check(run(e,d,r)==EventProcessorStatus::Error&&calls==0,"unknown decoder enum fails closed");}
 {auto e=event();Decoder d;Repo r;d.result.timeout_type=OperationTimeoutType::Unknown;calls=0;
 check(run(e,d,r)==EventProcessorStatus::Rejected&&calls==0,"UNKNOWN timeout type rejected");}
 {auto e=event();Decoder d;Repo r;d.result.dispatch_certainty=DispatchCertainty::Unknown;calls=0;
 check(run(e,d,r)==EventProcessorStatus::Rejected&&calls==0,"UNKNOWN dispatch certainty rejected");}
 struct M{OperationTimeoutResultStatus in;EventProcessorStatus out;};
 const M map[]={
 {OperationTimeoutResultStatus::Applied,EventProcessorStatus::NoStateChange},
 {OperationTimeoutResultStatus::Duplicate,EventProcessorStatus::Duplicate},
 {OperationTimeoutResultStatus::EventConflict,EventProcessorStatus::EventConflict},
 {OperationTimeoutResultStatus::InvalidEvent,EventProcessorStatus::Rejected},
 {OperationTimeoutResultStatus::EventNotReliable,EventProcessorStatus::Rejected},
 {OperationTimeoutResultStatus::WrongEventType,EventProcessorStatus::Rejected},
 {OperationTimeoutResultStatus::InvalidTimeoutType,EventProcessorStatus::Rejected},
 {OperationTimeoutResultStatus::InvalidDispatchCertainty,EventProcessorStatus::Rejected},
 {OperationTimeoutResultStatus::StaleEvent,EventProcessorStatus::Error},
 {OperationTimeoutResultStatus::EventOrderUnknown,EventProcessorStatus::Error},
 {OperationTimeoutResultStatus::OperationNotFound,EventProcessorStatus::Rejected},
 {OperationTimeoutResultStatus::CorrelationMismatch,EventProcessorStatus::Rejected},
 {OperationTimeoutResultStatus::TimeoutNotApplicable,EventProcessorStatus::Rejected},
 {OperationTimeoutResultStatus::AlreadyOutcomeUnknown,EventProcessorStatus::Rejected},
 {OperationTimeoutResultStatus::LateTimeoutIgnored,EventProcessorStatus::Rejected},
 {OperationTimeoutResultStatus::ConcurrentOperationUpdate,EventProcessorStatus::ConcurrentUpdate},
 {OperationTimeoutResultStatus::ConcurrentSourceUpdate,EventProcessorStatus::ConcurrentUpdate},
 {OperationTimeoutResultStatus::RepositoryError,EventProcessorStatus::Error},
 {static_cast<OperationTimeoutResultStatus>(255),EventProcessorStatus::Error}};
 for(const auto&m:map){auto e=event();Decoder d;Repo r;calls=0;f11_result={m.in,OperationLifecycleStatus::Failed,2};
 check(run(e,d,r)==m.out,"F11 result mapping");check(calls==1,"F11 called once after valid decode");}
 std::cout<<"Checks: "<<checks<<"\nFailures: "<<failures<<'\n';return failures==0?EXIT_SUCCESS:EXIT_FAILURE;
}
