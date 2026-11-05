#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>
#include "core/operations.hpp"
using namespace dockstation::core;
namespace {
int failures=0;
bool same_optional(const std::optional<std::string_view>& a,const std::optional<std::string_view>& b) noexcept {
 return a.has_value()==b.has_value() && (!a.has_value() || *a==*b);
}
bool same_request(const ActionRequest& a,const ActionRequest& b) noexcept {
 return a.operation_id==b.operation_id && a.correlation_id==b.correlation_id &&
        a.target_module==b.target_module && a.action==b.action && same_optional(a.parameters,b.parameters);
}
class FakeRepository final:public StateRepository {
public:
 OperationIntentSaveStatus save_operation_intent(const ActionRequest&) noexcept override {return OperationIntentSaveStatus::Saved;}
};
class IdempotentFakeDispatcher final:public ActionDispatcher {
public:
 std::optional<ActionRequest> known{}; int calls=0,executions=0;
 ActionDispatchPortStatus dispatch(const ActionRequest& r) noexcept override {
  ++calls;
  if(!known){known=r;++executions;return ActionDispatchPortStatus::Accepted;}
  if(known->operation_id==r.operation_id)
   return same_request(*known,r)?ActionDispatchPortStatus::AlreadyKnown:ActionDispatchPortStatus::Conflict;
  known=r;++executions;return ActionDispatchPortStatus::Accepted;
 }
};
class ScriptedDispatcher final:public ActionDispatcher {
public:
 ActionDispatchPortStatus next{ActionDispatchPortStatus::Accepted}; int calls=0;
 ActionDispatchPortStatus dispatch(const ActionRequest&) noexcept override {++calls;return next;}
};
void expect(bool ok,std::string_view name){std::cout<<(ok?"PASS":"FAIL")<<" - "<<name<<'\n';if(!ok)++failures;}
}
int main(){
 const ActionRequest request{"operation-101","event-201","lid_manager","close_lid","{\"mode\":\"normal\"}"};
 FakeRepository repo; const PersistResult saved=persist_operation_intent(request,repo);
 expect(saved.status==PersistStatus::Persisted,"F07 returns a new persisted result");
 expect(saved.authorizes(request),"persisted result authorizes exact request");
 IdempotentFakeDispatcher receiver;
 auto result=dispatch_action_request(request,saved,receiver);
 expect(result.status==DispatchStatus::Accepted,"confirmed dispatch is reported as accepted");
 expect(receiver.calls==1&&receiver.executions==1,"valid persisted request reaches target exactly once");
 expect(receiver.known&&same_request(*receiver.known,request),"all request fields reach dispatcher unchanged");
 result=dispatch_action_request(request,saved,receiver);
 expect(result.status==DispatchStatus::AlreadyKnown,"duplicate operation is recognized by receiver");
 expect(receiver.executions==1,"duplicate delivery does not start a second physical operation");
 const ActionRequest changed{"operation-101","event-201","lid_manager","open_lid","{\"mode\":\"normal\"}"};
 expect(dispatch_action_request(changed,saved,receiver).status==DispatchStatus::PersistenceReceiptMismatch,
        "receipt prevents replacing action under same operation ID");
 const int before=receiver.calls;
 const PersistResult already{PersistStatus::AlreadyPersisted,{}}, conflict{PersistStatus::Conflict,{}},
                     error{PersistStatus::RepositoryError,{}};
 expect(dispatch_action_request(request,already,receiver).status==DispatchStatus::PersistenceNotConfirmed,
        "already persisted request is not automatically resent");
 expect(dispatch_action_request(request,conflict,receiver).status==DispatchStatus::PersistenceNotConfirmed,
        "persistence conflict blocks dispatch");
 expect(dispatch_action_request(request,error,receiver).status==DispatchStatus::PersistenceNotConfirmed,
        "repository error blocks dispatch");
 expect(receiver.calls==before,"blocked persistence results never call dispatcher");
 const PersistResult unrelated{PersistStatus::Persisted,request};
 expect(dispatch_action_request(changed,unrelated,receiver).status==DispatchStatus::PersistenceReceiptMismatch,
        "persisted receipt must match every request field");
 const ActionRequest no_params{"operation-102","event-202","lid_manager","close_lid",std::nullopt};
 const PersistResult no_params_receipt{PersistStatus::Persisted,no_params};
 const ActionRequest empty_params{"operation-102","event-202","lid_manager","close_lid",std::string_view{""}};
 expect(dispatch_action_request(empty_params,no_params_receipt,receiver).status==DispatchStatus::PersistenceReceiptMismatch,
        "empty parameters do not match absent parameters");
 const ActionRequest invalid{"","event-202","lid_manager","close_lid",std::nullopt};
 expect(dispatch_action_request(invalid,saved,receiver).status==DispatchStatus::InvalidRequest,
        "empty operation ID is rejected");
 expect(receiver.calls==before,"invalid request does not reach dispatcher");
 ScriptedDispatcher scripted; scripted.next=ActionDispatchPortStatus::Rejected;
 expect(dispatch_action_request(request,saved,scripted).status==DispatchStatus::Rejected,"confirmed rejection is propagated");
 scripted.next=ActionDispatchPortStatus::Conflict;
 expect(dispatch_action_request(request,saved,scripted).status==DispatchStatus::Conflict,"operation ID conflict is propagated");
 scripted.next=ActionDispatchPortStatus::Unknown;
 expect(dispatch_action_request(request,saved,scripted).status==DispatchStatus::OutcomeUnknown,"uncertain result remains unknown");
 scripted.next=static_cast<ActionDispatchPortStatus>(255);
 expect(dispatch_action_request(request,saved,scripted).status==DispatchStatus::OutcomeUnknown,"unknown port status fails closed");
 std::cout<<(failures==0?"All tests passed":"Test failures: ")<<failures<<'\n';
 return failures==0?EXIT_SUCCESS:EXIT_FAILURE;
}