#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include "configuration/configuration_manager.hpp"
using namespace dockstation::configuration;
namespace {
int failures=0, checks=0;
void expect(bool ok,const char* name){++checks;if(ok)std::cout<<"PASS - "<<name<<'\n';else{std::cerr<<"FAIL - "<<name<<'\n';++failures;}}
ConfigurationValueView si(std::int64_t n){ConfigurationValueView v{};v.type=ConfigurationValueType::SignedInteger;v.value.signed_integer_value=n;return v;}
ConfigurationValueView num(double n){ConfigurationValueView v{};v.type=ConfigurationValueType::Number;v.value.number_value=n;return v;}
ConfigurationValueView str(std::string_view s){ConfigurationValueView v{};v.type=ConfigurationValueType::String;v.value.string_value=s;return v;}
ConfigurationValueView obj(const ConfigurationObjectEntryView* e,std::size_t n){ConfigurationValueView v{};v.type=ConfigurationValueType::Object;v.value.object_value={e,n};return v;}
bool has(const ConfigurationValidationResult& r,std::string_view code,std::string_view path){
 if(!r.issues.items)return false;for(std::size_t i=0;i<r.issues.size;++i)
 if(r.issues.items[i].reason_code==code&&r.issues.items[i].field_path&&*r.issues.items[i].field_path==path)return true;
 return false;
}
}
int main(){
 constexpr std::string_view mode[]={"station","mode"}, limit[]={"station","limit"}, temp[]={"station","temperature"};
 const ConfigurationValueView modes[]={str("AUTO"),str("SERVICE")};
 const ConfigurationValueRuleView rs[]={
  {{mode,2,"station.mode"},ConfigurationValueType::String,false,{},false,{},modes,2},
  {{limit,2,"station.limit"},ConfigurationValueType::SignedInteger,true,si(1),true,si(5),nullptr,0},
  {{temp,2,"station.temperature"},ConfigurationValueType::Number,true,num(-30),true,num(50),nullptr,0}};
 const ConfigurationValueRuleSetView policy{rs,3};
 const ConfigurationObjectEntryView good[]={{"mode",str("AUTO")},{"limit",si(3)},{"temperature",num(20)}};
 const ConfigurationObjectEntryView root[]={{"station",obj(good,3)}};
 ConfigurationCandidate candidate{};candidate.document.root=obj(root,1);ConfigurationIssue issues[4]{};
 auto r=validate_configuration_values(candidate,policy,issues,4);
 expect(r.status==ConfigurationValidationStatus::Valid,"valid explicit type/enum/range rules");
 const ConfigurationObjectEntryView badtype[]={{"mode",si(2)},{"limit",si(3)},{"temperature",num(20)}};
 const ConfigurationObjectEntryView rt[]={{"station",obj(badtype,3)}};candidate.document.root=obj(rt,1);
 r=validate_configuration_values(candidate,policy,issues,4);
 expect(has(r,"CONFIGURATION_VALUE_TYPE_MISMATCH","station.mode"),"wrong type rejected");
 const ConfigurationObjectEntryView badenum[]={{"mode",str("FLIGHT")},{"limit",si(3)},{"temperature",num(20)}};
 const ConfigurationObjectEntryView re[]={{"station",obj(badenum,3)}};candidate.document.root=obj(re,1);
 r=validate_configuration_values(candidate,policy,issues,4);
 expect(has(r,"CONFIGURATION_VALUE_NOT_ALLOWED","station.mode"),"unknown enum rejected");
 const ConfigurationObjectEntryView low[]={{"mode",str("AUTO")},{"limit",si(0)},{"temperature",num(20)}};
 const ConfigurationObjectEntryView rl[]={{"station",obj(low,3)}};candidate.document.root=obj(rl,1);
 r=validate_configuration_values(candidate,policy,issues,4);
 expect(has(r,"CONFIGURATION_VALUE_BELOW_MINIMUM","station.limit"),"below-minimum value rejected");
 const ConfigurationObjectEntryView high[]={{"mode",str("AUTO")},{"limit",si(6)},{"temperature",num(51)}};
 const ConfigurationObjectEntryView rh[]={{"station",obj(high,3)}};candidate.document.root=obj(rh,1);
 r=validate_configuration_values(candidate,policy,issues,4);
 expect(has(r,"CONFIGURATION_VALUE_ABOVE_MAXIMUM","station.limit")&&has(r,"CONFIGURATION_VALUE_ABOVE_MAXIMUM","station.temperature"),"above-maximum values reported");
 const ConfigurationObjectEntryView empty[]={{"station",obj(nullptr,0)}};candidate.document.root=obj(empty,1);
 r=validate_configuration_values(candidate,policy,issues,4);
 expect(r.status==ConfigurationValidationStatus::Invalid&&r.issues.size==3,"all missing paths reported");
 const ConfigurationObjectEntryView dup[]={{"mode",str("AUTO")},{"mode",str("SERVICE")},{"limit",si(3)},{"temperature",num(20)}};
 const ConfigurationObjectEntryView rd[]={{"station",obj(dup,4)}};candidate.document.root=obj(rd,1);
 r=validate_configuration_values(candidate,policy,issues,4);
 expect(has(r,"CONFIGURATION_VALUE_PATH_AMBIGUOUS","station.mode"),"duplicate keys rejected as ambiguous");
 candidate.document.root=obj(root,1);
 expect(validate_configuration_values(candidate,policy,issues,2).status==ConfigurationValidationStatus::InternalError,"small issue buffer fails closed");
 expect(validate_configuration_values(candidate,policy,nullptr,0).status==ConfigurationValidationStatus::Valid,"null issue buffer allowed when no issues");
 const ConfigurationObjectEntryView nanv[]={{"mode",str("AUTO")},{"limit",si(3)},{"temperature",num(std::numeric_limits<double>::quiet_NaN())}};
 const ConfigurationObjectEntryView rn[]={{"station",obj(nanv,3)}};candidate.document.root=obj(rn,1);
 r=validate_configuration_values(candidate,policy,issues,4);
 expect(has(r,"CONFIGURATION_VALUE_NOT_FINITE","station.temperature"),"NaN rejected");
 expect(validate_configuration_values(candidate,{},issues,4).status==ConfigurationValidationStatus::InternalError,"empty policy rejected");
 const ConfigurationValueRuleView badtype_rule[]={{{mode,2,"station.mode"},ConfigurationValueType::Unknown}};
 expect(validate_configuration_values(candidate,{badtype_rule,1},issues,4).status==ConfigurationValidationStatus::InternalError,"Unknown expected type rejected");
 const ConfigurationValueView dupenum[]={str("AUTO"),str("AUTO")};
 const ConfigurationValueRuleView dupenum_rule[]={{{mode,2,"station.mode"},ConfigurationValueType::String,false,{},false,{},dupenum,2}};
 expect(validate_configuration_values(candidate,{dupenum_rule,1},issues,4).status==ConfigurationValidationStatus::InternalError,"duplicate enum policy rejected");
 const ConfigurationValueRuleView inverted[]={{{limit,2,"station.limit"},ConfigurationValueType::SignedInteger,true,si(5),true,si(1),nullptr,0}};
 expect(validate_configuration_values(candidate,{inverted,1},issues,4).status==ConfigurationValidationStatus::InternalError,"inverted range policy rejected");
 const ConfigurationValueRuleView wrongbound[]={{{limit,2,"station.limit"},ConfigurationValueType::SignedInteger,true,num(1),false,{},nullptr,0}};
 expect(validate_configuration_values(candidate,{wrongbound,1},issues,4).status==ConfigurationValidationStatus::InternalError,"bound type mismatch rejected");
 std::cout<<"\n"<<(failures==0?"PASS":"FAIL")<<" - "<<checks<<" checks; failures="<<failures<<'\n';
 return failures==0?EXIT_SUCCESS:EXIT_FAILURE;
}