#include "controller_station_safety_input_adapter.hpp"

#include <cstdlib>
#include <iostream>

using namespace dockstation::firmware::esp32;

namespace {
int checks=0, failures=0;
void expect(bool ok,const char* label){++checks;if(ok)std::cout<<"PASS - "<<label<<'\n';else{++failures;std::cerr<<"FAIL - "<<label<<'\n';}}
LidLimitSwitchConfig config(){return {{true,LimitSwitchActiveLevel::High,10U},{true,LimitSwitchActiveLevel::Low,10U}};}
}

int main(){
    ControllerStationSafetyInputAdapter adapter;
    ControllerStationSafetyInputs inputs{};
    inputs.platform_motion_active=true;
    auto result=adapter.update(inputs,true,false,0U);
    expect(result.lid_position==LidPositionStatus::NotConfigured,"unconfigured end-stops fail closed as NOT_CONFIGURED");
    expect(result.platform_motion_active.has_value() &&
               *result.platform_motion_active,
           "adapter preserves unrelated, explicitly active safety inputs");

    adapter.configure(config());
    result=adapter.update(inputs,true,true,100U);
    expect(result.lid_position==LidPositionStatus::Transitioning,"first raw sample is not trusted before debounce");
    result=adapter.update(inputs,true,true,110U);
    expect(result.lid_position==LidPositionStatus::Open,"stable open switch maps to OPEN");

    result=adapter.update(inputs,false,true,111U);
    expect(result.lid_position==LidPositionStatus::Transitioning,"leaving endpoint immediately removes confirmed position");
    result=adapter.update(inputs,false,true,121U);
    expect(result.lid_position==LidPositionStatus::Transitioning,"between endpoints never fabricates OPEN or CLOSED");

    result=adapter.update(inputs,false,false,122U);
    result=adapter.update(inputs,false,false,132U);
    expect(result.lid_position==LidPositionStatus::Closed,"stable closed switch maps to CLOSED");

    result=adapter.update(inputs,true,false,133U);
    result=adapter.update(inputs,true,false,143U);
    expect(result.lid_position==LidPositionStatus::Unknown,"contradictory active end-stops map to UNKNOWN");

    result=adapter.update(inputs,std::nullopt,false,144U);
    expect(result.lid_position==LidPositionStatus::Unknown,"unavailable GPIO sample maps to UNKNOWN");

    result=adapter.update(inputs,true,true,200U);
    expect(result.lid_position==LidPositionStatus::Transitioning,"recovery requires a fresh debounce interval");
    result=adapter.update(inputs,true,true,210U);
    expect(result.lid_position==LidPositionStatus::Open,"valid samples recover after debounce");

    result=adapter.update(inputs,true,true,190U);
    expect(adapter.last_lid_snapshot().update_status==LidLimitSwitchUpdateStatus::MonotonicTimeWentBackwards &&
           result.lid_position==LidPositionStatus::Unknown,"clock regression fails closed");

    adapter.configure({});
    result=adapter.update(inputs,true,false,300U);
    expect(result.lid_position==LidPositionStatus::NotConfigured,"reconfiguration resets tracker to NOT_CONFIGURED");

    std::cout<<"Checks: "<<checks<<", failures: "<<failures<<'\n';
    return failures==0?EXIT_SUCCESS:EXIT_FAILURE;
}
