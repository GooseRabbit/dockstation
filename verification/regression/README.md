# Focused repository-contract regression checks

Run from the repository root with GCC or Clang supporting C++17:

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -Isrc \
  src/core/event_processing.cpp src/configuration/configuration_load.cpp \
  verification/regression/test_repository_contract_edges.cpp -o repository_contract_edges
./repository_contract_edges
```

The repository-contract edge harness verifies two fail-closed cases. Focused
regression tests live under `verification/regression/`; existing `tests/` programs
may be updated when their production contract changes, without rewriting unrelated suites:

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
  src/core/source_session_coordinator.cpp \
  verification/regression/test_source_session_contract.cpp -o source_session_contract
./source_session_contract
```

The CTest target is `source_session_contract`. It covers SSR-01…SSR-20
through the production `SourceSessionCoordinator`, with fake verifier/reader/
repository ports at the adapter boundaries. The test-only `source_session_test_support.hpp` constructs opaque evidence fixtures; production `SourceSessionEvidence` has a private constructor and is not an aggregate. The coordinator checks that
`Event.source` and `Event.source_instance` match the supplied evidence before
calling any trust port, then verifies the session, reads the cursor and asks
the repository to confirm it. The fake repository models compare-and-set and
registration-revision checks; it is not production persistence. The transport
adapter must still build Event and evidence from the authenticated channel
context. No real cryptography, provisioning, durable storage across process
restart, or ESP32 transport is tested.
