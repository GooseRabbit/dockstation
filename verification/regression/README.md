# Focused repository-contract regression checks

Run from the repository root with GCC or Clang supporting C++17:

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -Isrc \
  src/core/event_processing.cpp src/configuration/configuration_load.cpp \
  verification/regression/test_repository_contract_edges.cpp -o repository_contract_edges
./repository_contract_edges
```

The harness verifies two fail-closed edge cases without modifying the existing
`tests/` programs or `tools/` runner:

- `check_duplicate_event()` accepts `NOT_FOUND` as `NEW` only when the
  repository's returned `Event` is completely empty.
- `load_configuration()` treats a non-null data pointer or non-zero record
  length accompanying `UNAVAILABLE` or `READ_ERROR` as a contradictory
  repository result.

The CFG-F05 and CFG-F06 symbols are focused test doubles solely to satisfy
linkage. The test cases return on the repository status and never invoke them.
The harness does not replace the existing unit/contract tests or test real
storage adapters.
