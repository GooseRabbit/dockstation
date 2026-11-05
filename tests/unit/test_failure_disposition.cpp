#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include "core/failure_disposition.hpp"
using namespace dockstation::core;
int main() {
 int failures = 0;
 for (int i = 1; i <= 41; ++i) {
  char id[10];
  std::snprintf(id, sizeof(id), "FAIL-%03d", i);
  const auto result = resolve_failure_disposition({id});
  if (!result || result.failure_id != id || result.preliminary_reaction.empty()) {
   std::cerr << "FAIL known ID " << id << '\n'; ++failures;
  }
 }
 constexpr std::string_view unknown_ids[] = {"", "FAIL-000", "FAIL-042", "fail-001", "FAIL-1", "FAIL-001 "};
 for (const auto id : unknown_ids) {
  const auto result = resolve_failure_disposition({id});
  if (result || result.status != FailureDispositionStatus::UnknownFailure || !result.preliminary_reaction.empty()) {
   std::cerr << "FAIL unknown ID " << id << '\n'; ++failures;
  }
 }
 const auto first = resolve_failure_disposition({"FAIL-005"});
 const auto second = resolve_failure_disposition({"FAIL-005"});
 if (first.preliminary_reaction != second.preliminary_reaction) {
  std::cerr << "FAIL deterministic lookup\n"; ++failures;
 }
 constexpr int checks = 48;
 std::cout << (failures == 0 ? "PASS" : "FAIL") << " - " << checks << " checks; failures=" << failures << '\n';
 return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
