# Host unit tests

The StationStateManager host unit tests are intentionally independent of
Arduino APIs and physical hardware drivers. The code is written against C++17 as a
provisional portable implementation; the final language/toolchain decision is
still open in docs/12_DECISIONS_AND_OPEN_QUESTIONS.md.

## Run with GCC or Clang

From the repository root:

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp tests/unit/test_event_validation.cpp -o test_event_validation
    ./test_event_validation

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/state_transitions.cpp tests/unit/test_state_transitions.cpp -o test_state_transitions
    ./test_state_transitions

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_processing.cpp tests/unit/test_event_freshness.cpp -o test_event_freshness
    ./test_event_freshness

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_processing.cpp tests/unit/test_event_classification.cpp -o test_event_classification
    ./test_event_classification

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_processing.cpp tests/unit/test_event_duplicates.cpp -o test_event_duplicates
    ./test_event_duplicates

On Windows, use .exe for the output name and run ./test_event_validation.exe
from PowerShell if a compatible GCC/Clang toolchain is installed.

## Scope

The tests cover the common event envelope only: required fields, documented
quality statuses, conditional source/operation fields, and optional reason
metadata. The validator does not parse timestamps or payload contents, verify
that a schema version is supported, check event_id uniqueness, or determine
whether an event is fresh. Those require contracts/catalogues or later SSM
functions and are deliberately outside this first task.

The host tests do not constitute an ESP32/Arduino target build or hardware test.

## State transition tests

The `resolve_transition()` tests cover the three approved startup transitions from §8.1 of `docs/17_STATION_STATE_MANAGER.md`. They verify that configuration validation, a complete positive critical-diagnostics report, a confirmed safe initial condition, and the absence of a pending recovery requirement are mandatory before entering WAITING. Every other state/event pair is rejected. These tests do not validate the full station transition matrix, which is not yet approved.


## Event freshness tests

The `check_event_freshness()` tests compare source sequence numbers only within the same source/session. The cursor must be explicitly bound to the same source and session, even for the first event. A missing or repeated sequence, unmatched source, or unconfirmed/mismatched session is UNORDERED; a regressed sequence is STALE; a first or strictly advanced sequence is CURRENT. Calendar timestamps are never used as a substitute for source sequence numbers. When a new source session is confirmed by the source layer, that layer must reset the cursor and store the new session identifier before processing its first event. This function does not detect duplicate `event_id` values; that is a separate SSM task.


## Event classification tests

The `classify_event()` tests cover all ten registered event types from §2.1 of
`docs/06_INTERFACES_AND_DATA_CONTRACTS.md`, all six categories, and the
`Unknown` result for empty, unregistered, case-mismatched, and near-match
event types. Repeated calls with the same event must return the same category.
Classification uses exact `event_type` matching only; it does not validate the
event envelope, change station state, authorize an operation, or trigger a
physical action.


## Duplicate event tests

The `check_duplicate_event()` tests use an in-memory fake `EventRepository`.
They cover an absent event ID, exact duplicate, a changed receiver-assigned
`received_at`, changes to every other event-content field, repository errors,
a contradictory repository key, unknown repository status, and read-only/
deterministic behavior. The function does not write or reserve new event IDs.
Atomic coordination between deduplication and later side effects is not covered
by this unit function and must be handled by the event-processing orchestration.


## Action request creation tests

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/operations.cpp tests/unit/test_action_request.cpp -o test_action_request
    ./test_action_request

The `create_action_request()` tests use a fake `OperationIdProvider`. They cover
valid field mapping, required logical fields, optional/empty parameters, provider
failure, empty IDs, repeated calls with distinct provider values, and unchanged
input. The test does not prove uniqueness of a production ID algorithm: that is
the provider's contract and must be tested against the selected persistent
implementation. It also does not test storage, dispatch, parameter serialization,
or physical hardware.


## Operation intent persistence tests

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/operations.cpp tests/unit/test_operation_intent.cpp -o test_operation_intent
    ./test_operation_intent

The `persist_operation_intent()` tests use a fake `StateRepository`. They cover
new persistence, exact-existing status, conflicts, repository errors, unknown
statuses, required fields, optional/empty parameters, and dispatch gating.
The fake does not prove crash consistency, atomicity, flash wear behavior, or
power-loss durability; those require the selected storage adapter and hardware.
