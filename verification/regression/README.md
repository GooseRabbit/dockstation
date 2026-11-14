# Focused repository-contract regression checks

Run from the repository root with GCC or Clang supporting C++17:

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -Isrc \
  src/core/event_processing.cpp src/configuration/configuration_load.cpp \
  verification/regression/test_repository_contract_edges.cpp -o repository_contract_edges
./repository_contract_edges
```

The repository-contract edge harness verifies two fail-closed cases without
modifying the existing `tests/` programs or `tools/` runner:

- `check_duplicate_event()` accepts `NOT_FOUND` as `NEW` only when the
  repository's returned `Event` is completely empty.
- `load_configuration()` treats a non-null data pointer or non-zero record
  length accompanying `UNAVAILABLE` or `READ_ERROR` as a contradictory
  repository result.

A separate `test_source_cursor_reader.cpp` harness is registered with CTest as
`source_cursor_reader`. It runs 25 host checks against a test-only reader that
owns its string storage. It covers Found/NotFound/Error, response-shape
validation, unknown statuses, exact source matching and string-view lifetime.
It is not a production storage adapter and does not test persistence across
process restarts or transaction commits.

Run it manually from the repository root:

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -Isrc verification/regression/test_source_cursor_reader.cpp src/core/event_processing.cpp -o source_cursor_reader_contract
./source_cursor_reader_contract
```

The CFG-F05 and CFG-F06 symbols are focused test doubles solely to satisfy
linkage. The test cases return on the repository status and never invoke them.
The harness does not replace the existing unit/contract tests or test real
storage adapters.


## Source-session contract harness

Run from the repository root:

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -Isrc \\
  src/core/event_processing.cpp src/core/source_session.cpp \
  verification/regression/test_source_session_contract.cpp -o source_session_contract
./source_session_contract
```

The CTest target is `source_session_contract`. It covers SSR-01…SSR-18
with fake verifier/reader/repository ports and result-shape validators. The
fake repository models the required compare-and-set and registration-revision
checks; it is not production persistence. The event/session binding scenario
uses a test-local predicate because the production event-adapter binding and
composition-root wiring are still open. No cryptography, provisioning,
durability across process restart, or ESP32 transport is tested.
