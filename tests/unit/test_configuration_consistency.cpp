#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include "configuration/configuration_manager.hpp"
using namespace dockstation::configuration;
namespace {
int failures=0, checks=0;
void expect(bool ok,const char* name){++checks;if(ok)std::cout<<"PASS - "<<name<<'\n';else{std::cerr<<"FAIL - "<<name<<'\n';++failures;}}
ConfigurationValueView num(double n){ConfigurationValueView v{};v.type=ConfigurationValueType::Number;v.value.number_value=n;return v;}
ConfigurationValueView si(std::int64_t n){ConfigurationValueView v{};v.type=ConfigurationValueType::SignedInteger;v.value.signed_integer_value=n;return v;}
ConfigurationValueView ui(std::uint64_t n){ConfigurationValueView v{};v.type=ConfigurationValueType::UnsignedInteger;v.value.unsigned_integer_value=n;return v;}
ConfigurationValueView boolean(bool b){ConfigurationValueView v{};v.type=ConfigurationValueType::Boolean;v.value.boolean_value=b;return v;}
ConfigurationValueView str(std::string_view s){ConfigurationValueView v{};v.type=ConfigurationValueType::String;v.value.string_value=s;return v;}
ConfigurationValueView obj(const ConfigurationObjectEntryView* e,std::size_t n){ConfigurationValueView v{};v.type=ConfigurationValueType::Object;v.value.object_value={e,n};return v;}
bool has(const ConfigurationValidationResult& r,std::string_view code,std::string_view path){
 if(!r.issues.items)return false;
 for(std::size_t i=0;i<r.issues.size;++i)if(r.issues.items[i].reason_code==code&&r.issues.items[i].field_path&&*r.issues.items[i].field_path==path)return true;
 return false;
}
constexpr std::string_view minp[]={"power","minimum"}, maxp[]={"power","maximum"};
constexpr std::string_view modep[]={"station","mode"}, modecp[]={"station","mode_copy"};
constexpr std::string_view enablep[]={"station","enabled"}, enablecp[]={"station","enabled_copy"};
ConfigurationConsistencyRuleView rule(const std::string_view* l,std::size_t ln,const std::string_view* r,std::size_t rn,
 ConfigurationValueType t,ConfigurationConsistencyOperator op,std::string_view d){
 return {{l,ln,"left"},{r,rn,"right"},t,op,d};
}
ConfigurationCandidate valid_candidate(){
 static const ConfigurationObjectEntryView power[]={{"minimum",num(5)},{"maximum",num(10)}};
 static const ConfigurationObjectEntryView station[]={{"mode",str("AUTO")},{"mode_copy",str("AUTO")},{"enabled",boolean(true)},{"enabled_copy",boolean(false)}};
 static const ConfigurationObjectEntryView root[]={{"power",obj(power,2)},{"station",obj(station,4)}};
 ConfigurationCandidate c{};c.document.root=obj(root,2);return c;
}
}
int main(){
 const ConfigurationConsistencyRuleView rules[]={
  rule(minp,2,maxp,2,ConfigurationValueType::Number,ConfigurationConsistencyOperator::LessThanOrEqual,"power.minimum <= power.maximum"),
  rule(modep,2,modecp,2,ConfigurationValueType::String,ConfigurationConsistencyOperator::Equal,"station.mode == station.mode_copy"),
  rule(enablep,2,enablecp,2,ConfigurationValueType::Boolean,ConfigurationConsistencyOperator::NotEqual,"station.enabled != station.enabled_copy")};
 const ConfigurationConsistencyRuleSetView policy{rules,3};ConfigurationIssue issues[4]{};auto c=valid_candidate();
 auto result=validate_configuration_consistency(c,policy,issues,4);
 expect(result.status==ConfigurationValidationStatus::Valid,"valid numeric, string and boolean rules pass");
 expect(result.issues.size==0&&result.issues.items==nullptr,"valid result has no issues");
 expect(validate_configuration_consistency(c,policy,nullptr,0).status==ConfigurationValidationStatus::Valid,"null buffer allowed with no issues");
 const auto lt=rule(minp,2,maxp,2,ConfigurationValueType::Number,ConfigurationConsistencyOperator::LessThan,"min < max");
 expect(validate_configuration_consistency(c,{&lt,1},issues,4).status==ConfigurationValidationStatus::Valid,"strict less-than accepts ordered numbers");
 const ConfigurationObjectEntryView eqp[]={{"minimum",num(10)},{"maximum",num(10)}};
 const ConfigurationObjectEntryView eqr[]={{"power",obj(eqp,2)}};c.document.root=obj(eqr,1);
 expect(validate_configuration_consistency(c,{&lt,1},issues,4).status==ConfigurationValidationStatus::Invalid,"strict less-than rejects equal operands");
 const auto gt=rule(maxp,2,minp,2,ConfigurationValueType::SignedInteger,ConfigurationConsistencyOperator::GreaterThan,"max > min");
 const ConfigurationObjectEntryView ip[]={{"minimum",si(5)},{"maximum",si(10)}};
 const ConfigurationObjectEntryView ir[]={{"power",obj(ip,2)}};c.document.root=obj(ir,1);
 expect(validate_configuration_consistency(c,{&gt,1},issues,4).status==ConfigurationValidationStatus::Valid,"signed greater-than passes");
 const ConfigurationObjectEntryView iep[]={{"minimum",si(5)},{"maximum",si(5)}};
 const ConfigurationObjectEntryView ier[]={{"power",obj(iep,2)}};c.document.root=obj(ier,1);
 expect(validate_configuration_consistency(c,{&gt,1},issues,4).status==ConfigurationValidationStatus::Invalid,"strict greater-than rejects equality");
 const auto ge=rule(maxp,2,minp,2,ConfigurationValueType::UnsignedInteger,ConfigurationConsistencyOperator::GreaterThanOrEqual,"max >= min");
 const ConfigurationObjectEntryView uep[]={{"minimum",ui(5)},{"maximum",ui(5)}};
 const ConfigurationObjectEntryView uer[]={{"power",obj(uep,2)}};c.document.root=obj(uer,1);
 expect(validate_configuration_consistency(c,{&ge,1},issues,4).status==ConfigurationValidationStatus::Valid,"greater-or-equal accepts equal unsigned operands");
 const ConfigurationObjectEntryView badp[]={{"minimum",num(11)},{"maximum",num(10)}};
 const ConfigurationObjectEntryView goods[]={{"mode",str("AUTO")},{"mode_copy",str("AUTO")},{"enabled",boolean(true)},{"enabled_copy",boolean(false)}};
 const ConfigurationObjectEntryView badroot[]={{"power",obj(badp,2)},{"station",obj(goods,4)}};c.document.root=obj(badroot,2);
 result=validate_configuration_consistency(c,policy,issues,4);
 expect(result.status==ConfigurationValidationStatus::Invalid&&has(result,"CONFIGURATION_CONSISTENCY_NOT_SATISFIED","power.minimum <= power.maximum"),"numeric contradiction reported");
 const ConfigurationObjectEntryView diffs[]={{"mode",str("AUTO")},{"mode_copy",str("SERVICE")},{"enabled",boolean(true)},{"enabled_copy",boolean(false)}};
 const ConfigurationObjectEntryView diffroot[]={{"power",obj(badp,2)},{"station",obj(diffs,4)}};c.document.root=obj(diffroot,2);
 result=validate_configuration_consistency(c,policy,issues,4);
 expect(has(result,"CONFIGURATION_CONSISTENCY_NOT_SATISFIED","station.mode == station.mode_copy"),"unequal strings violate equality");
 const ConfigurationObjectEntryView eqbools[]={{"mode",str("AUTO")},{"mode_copy",str("AUTO")},{"enabled",boolean(true)},{"enabled_copy",boolean(true)}};
 const ConfigurationObjectEntryView eqboolroot[]={{"power",obj(badp,2)},{"station",obj(eqbools,4)}};c.document.root=obj(eqboolroot,2);
 result=validate_configuration_consistency(c,policy,issues,4);
 expect(has(result,"CONFIGURATION_CONSISTENCY_NOT_SATISFIED","station.enabled != station.enabled_copy"),"equal booleans violate inequality");
 const ConfigurationObjectEntryView wrong[]={{"mode",si(1)},{"mode_copy",str("AUTO")},{"enabled",boolean(true)},{"enabled_copy",boolean(false)}};
 const ConfigurationObjectEntryView wrongroot[]={{"power",obj(badp,2)},{"station",obj(wrong,4)}};c.document.root=obj(wrongroot,2);
 result=validate_configuration_consistency(c,policy,issues,4);
 expect(has(result,"CONFIGURATION_CONSISTENCY_TYPE_MISMATCH","station.mode == station.mode_copy"),"runtime type mismatch rejected");
 const ConfigurationObjectEntryView ml[]={{"maximum",num(10)}};
 const ConfigurationObjectEntryView mlr[]={{"power",obj(ml,1)},{"station",obj(goods,4)}};c.document.root=obj(mlr,2);
 result=validate_configuration_consistency(c,policy,issues,4);
 expect(has(result,"CONFIGURATION_CONSISTENCY_LEFT_FIELD_MISSING","power.minimum <= power.maximum"),"missing left operand reported");
 const ConfigurationObjectEntryView mr[]={{"minimum",num(5)}};
 const ConfigurationObjectEntryView mrr[]={{"power",obj(mr,1)},{"station",obj(goods,4)}};c.document.root=obj(mrr,2);
 result=validate_configuration_consistency(c,policy,issues,4);
 expect(has(result,"CONFIGURATION_CONSISTENCY_RIGHT_FIELD_MISSING","power.minimum <= power.maximum"),"missing right operand reported");
 const ConfigurationObjectEntryView noobj[]={{"power",str("disabled")}};c.document.root=obj(noobj,1);
 result=validate_configuration_consistency(c,{rules,1},issues,4);
 expect(has(result,"CONFIGURATION_CONSISTENCY_LEFT_PATH_UNRESOLVABLE","power.minimum <= power.maximum"),"non-object parent is unresolvable");
 const ConfigurationObjectEntryView dup[]={{"minimum",num(5)},{"minimum",num(6)},{"maximum",num(10)}};
 const ConfigurationObjectEntryView dupr[]={{"power",obj(dup,3)}};c.document.root=obj(dupr,1);
 result=validate_configuration_consistency(c,{rules,1},issues,4);
 expect(has(result,"CONFIGURATION_CONSISTENCY_LEFT_PATH_AMBIGUOUS","power.minimum <= power.maximum"),"duplicate key is ambiguous");
 ConfigurationValueView malformed{};malformed.type=ConfigurationValueType::Object;malformed.value.object_value={nullptr,1};c.document.root=malformed;
 result=validate_configuration_consistency(c,{rules,1},issues,4);
 expect(result.status==ConfigurationValidationStatus::InternalError&&result.issues.items[0].reason_code=="CONFIGURATION_CONSISTENCY_DOCUMENT_INVALID","malformed object view is internal error");
 const ConfigurationObjectEntryView nanp[]={{"minimum",num(std::numeric_limits<double>::quiet_NaN())},{"maximum",num(10)}};
 const ConfigurationObjectEntryView nanr[]={{"power",obj(nanp,2)}};c.document.root=obj(nanr,1);
 result=validate_configuration_consistency(c,{rules,1},issues,4);
 expect(has(result,"CONFIGURATION_CONSISTENCY_VALUE_NOT_FINITE","power.minimum <= power.maximum"),"non-finite number rejected");
 expect(validate_configuration_consistency(c,{nullptr,0},issues,4).status==ConfigurationValidationStatus::InternalError,"empty policy rejected");
 expect(validate_configuration_consistency(c,{rules,0},issues,4).status==ConfigurationValidationStatus::InternalError,"non-null zero-sized policy is rejected");
 const auto objectrule=rule(modep,2,modecp,2,ConfigurationValueType::Object,ConfigurationConsistencyOperator::Equal,"object equality");
 expect(validate_configuration_consistency(c,{&objectrule,1},issues,4).status==ConfigurationValidationStatus::InternalError,"non-scalar expected type invalid");
 const auto stringorder=rule(modep,2,modecp,2,ConfigurationValueType::String,ConfigurationConsistencyOperator::LessThan,"string order");
 expect(validate_configuration_consistency(c,{&stringorder,1},issues,4).status==ConfigurationValidationStatus::InternalError,"ordering requires numeric type");
 const auto same=rule(modep,2,modep,2,ConfigurationValueType::String,ConfigurationConsistencyOperator::Equal,"same path");
 expect(validate_configuration_consistency(c,{&same,1},issues,4).status==ConfigurationValidationStatus::InternalError,"self-comparison is invalid");
 const auto emptydiag=rule(modep,2,modecp,2,ConfigurationValueType::String,ConfigurationConsistencyOperator::Equal,"");
 expect(validate_configuration_consistency(c,{&emptydiag,1},issues,4).status==ConfigurationValidationStatus::InternalError,"empty diagnostic path invalid");
 const auto da=rule(modep,2,modecp,2,ConfigurationValueType::String,ConfigurationConsistencyOperator::Equal,"mode equality");
 const auto db=rule(modep,2,modecp,2,ConfigurationValueType::String,ConfigurationConsistencyOperator::Equal,"duplicate mode equality");
 const ConfigurationConsistencyRuleView dups[]={da,db};
 expect(validate_configuration_consistency(c,{dups,2},issues,4).status==ConfigurationValidationStatus::InternalError,"duplicate relation invalid");
 const auto unknown=rule(modep,2,modecp,2,ConfigurationValueType::String,static_cast<ConfigurationConsistencyOperator>(255),"bad operator");
 expect(validate_configuration_consistency(c,{&unknown,1},issues,4).status==ConfigurationValidationStatus::InternalError,"unknown operator rejected");
 c=valid_candidate();
 ConfigurationIssue sentinel{"SENTINEL",std::nullopt,ConfigurationIssueSeverity::Warning,"unchanged"};
 ConfigurationIssue small[1]={sentinel};
 const ConfigurationObjectEntryView bads[]={{"mode",str("AUTO")},{"mode_copy",str("SERVICE")},{"enabled",boolean(true)},{"enabled_copy",boolean(true)}};
 const ConfigurationObjectEntryView allbad[]={{"power",obj(badp,2)},{"station",obj(bads,4)}};c.document.root=obj(allbad,2);
 result=validate_configuration_consistency(c,policy,small,1);
 expect(result.status==ConfigurationValidationStatus::InternalError&&result.issues.items[0].reason_code=="CONFIGURATION_CONSISTENCY_ISSUE_BUFFER_TOO_SMALL","small issue buffer fails closed");
 expect(small[0].reason_code=="SENTINEL","small buffer is not partially overwritten");
 expect(validate_configuration_consistency(c,policy,nullptr,0).status==ConfigurationValidationStatus::InternalError,"null buffer fails closed when issues exist");
 std::cout<<"\n"<<(failures==0?"PASS":"FAIL")<<" - "<<checks<<" checks; failures="<<failures<<'\n';
 return failures==0?EXIT_SUCCESS:EXIT_FAILURE;
}
