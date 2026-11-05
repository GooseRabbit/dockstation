#include <cstdlib>
#include <iostream>
#include <string_view>
#include "core/state_transitions.hpp"
using namespace dockstation::core;
namespace {
int failures = 0;
void expect(std::string_view name, const TransitionResult& result,
            TransitionStatus status, StationState next_state,
            std::string_view reason = {}) {
    const bool passed = result.status == status &&
        (status == TransitionStatus::Rejected
            ? !result.next_state.has_value() && result.reason_code == reason
            : result.next_state.has_value() && *result.next_state == next_state);
    std::cout << (passed ? "PASS" : "FAIL") << " - " << name << '\n';
    if (!passed) {
        ++failures;
        std::cout << "  actual status=" << static_cast<int>(result.status)
                  << " reason=" << result.reason_code << '\n';
    }
}
Event event(std::string_view type) {
    Event value{};
    value.event_type = type;
    return value;
}
}  // namespace
int main() {
    const auto startup = event("STARTUP_REQUESTED");
    expect("startup request", resolve_transition(StationState::PoweredOff, startup),
           TransitionStatus::Allowed, StationState::Startup);
    expect("repeated evaluation is deterministic",
           resolve_transition(StationState::PoweredOff, startup),
           TransitionStatus::Allowed, StationState::Startup);
    const auto initialized = event("INITIALIZATION_COMPLETED");
    expect("configuration not validated",
           resolve_transition(StationState::Startup, initialized),
           TransitionStatus::Rejected, StationState::PoweredOff,
           "CONFIGURATION_NOT_VALIDATED");
    TransitionContext config_ok{};
    config_ok.configuration_validated = true;
    expect("validated configuration",
           resolve_transition(StationState::Startup, initialized, config_ok),
           TransitionStatus::Allowed, StationState::SelfTest);
    const auto self_test = event("SELF_TEST_COMPLETED");
    TransitionContext context{};
    expect("missing diagnostic report",
           resolve_transition(StationState::SelfTest, self_test, context),
           TransitionStatus::Rejected, StationState::PoweredOff,
           "DIAGNOSTIC_REPORT_INCOMPLETE");
    context.diagnostic_report_complete = true;
    expect("critical checks not passed",
           resolve_transition(StationState::SelfTest, self_test, context),
           TransitionStatus::Rejected, StationState::PoweredOff,
           "CRITICAL_CHECKS_NOT_PASSED");
    context.critical_checks_passed = true;
    expect("safe initial condition unknown",
           resolve_transition(StationState::SelfTest, self_test, context),
           TransitionStatus::Rejected, StationState::PoweredOff,
           "SAFE_INITIAL_CONDITION_UNCONFIRMED");
    context.safe_initial_condition_confirmed = true;
    context.recovery_required = true;
    expect("recovery blocks waiting",
           resolve_transition(StationState::SelfTest, self_test, context),
           TransitionStatus::Rejected, StationState::PoweredOff,
           "RECOVERY_REQUIRED");
    context.recovery_required = false;
    expect("all startup conditions confirmed",
           resolve_transition(StationState::SelfTest, self_test, context),
           TransitionStatus::Allowed, StationState::Waiting);
    expect("unknown event is rejected",
           resolve_transition(StationState::PoweredOff, event("UNKNOWN_EVENT")),
           TransitionStatus::Rejected, StationState::PoweredOff,
           "TRANSITION_NOT_APPROVED");
    expect("wrong source state is rejected",
           resolve_transition(StationState::Waiting, startup),
           TransitionStatus::Rejected, StationState::PoweredOff,
           "TRANSITION_NOT_APPROVED");
    std::cout << "Tests failed: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
