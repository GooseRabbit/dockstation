# Host unit tests

The StationStateManager host unit tests are intentionally independent of
Arduino APIs and physical hardware drivers. The code is written against C++17 as a
provisional portable implementation; the final language/toolchain decision is
still open in docs/12_DECISIONS_AND_OPEN_QUESTIONS.md.

## Run with GCC or Clang

From the repository root:

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp tests/unit/test_event_validation.cpp -o test_event_validation
    ./test_event_validation

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp src/core/event_processing.cpp src/core/state_transitions.cpp tests/unit/test_state_transitions.cpp -o test_state_transitions
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

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp src/core/event_processing.cpp src/core/operations.cpp tests/unit/test_action_request.cpp -o test_action_request
    ./test_action_request

The `create_action_request()` tests use a fake `OperationIdProvider`. They cover
valid field mapping, required logical fields, optional/empty parameters, provider
failure, empty IDs, repeated calls with distinct provider values, and unchanged
input. The test does not prove uniqueness of a production ID algorithm: that is
the provider's contract and must be tested against the selected persistent
implementation. It also does not test storage, dispatch, parameter serialization,
or physical hardware.


## Operation intent persistence tests

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp src/core/event_processing.cpp src/core/operations.cpp tests/unit/test_operation_intent.cpp -o test_operation_intent
    ./test_operation_intent

The `persist_operation_intent()` tests use a fake `StateRepository`. They cover
new persistence, exact-existing status, conflicts, repository errors, unknown
statuses, required fields, optional/empty parameters, and the exact request receipt
that F08 must match before dispatch.
The fake does not prove crash consistency, atomicity, flash wear behavior, or
power-loss durability; those require the selected storage adapter and hardware.


## Action request dispatch tests

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp src/core/event_processing.cpp src/core/operations.cpp tests/unit/test_operation_dispatch.cpp -o test_operation_dispatch
    ./test_operation_dispatch

The `dispatch_action_request()` tests use a fake persisted-intent receipt and
fake dispatchers. They cover exact receipt matching, rejection of missing or
mismatched persistence proof, all dispatcher statuses, duplicate operation IDs,
and prevention of a second execution in an idempotent receiving-module fake.
These host tests do not verify real inter-module transport, durable receiver-side
deduplication, SafetyManager integration, or physical hardware.


## Operation lifecycle update tests

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp src/core/event_processing.cpp src/core/operations.cpp tests/unit/test_operation_update.cpp -o test_operation_update
    ./test_operation_update

The `handle_operation_update()` tests use a fake `OperationUpdateRepository`. They cover valid lifecycle transitions, rejected jumps, terminal and OUTCOME_UNKNOWN behavior, exact duplicate events, event-ID conflicts, invalid/unreliable envelopes, stale or unordered source sequences, unknown operations, correlation mismatches, parallel operations, and concurrent operation/cursor changes. The fake verifies the repository contract but does not prove real transaction durability, receiver-side event persistence, source-cursor recovery after reboot, or hardware integration. The adapter must translate the operation status into the typed `OperationUpdate`; the payload serialization format is not fixed.


## Global state transition commit tests

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp src/core/event_processing.cpp src/core/state_transitions.cpp tests/unit/test_transition_commit.cpp -o test_transition_commit
    ./test_transition_commit

The commit_transition() tests use fake StationStateRepository and StateChangedPublisher ports. They cover all three currently approved startup transitions; event-envelope and quality validation; F04 current/stale/unordered cursor checks including zero and maximum sequence boundaries; forwarding of the expected source cursor; the atomic repository cursor-conflict result; F05 prerequisites and target matching; empty transition IDs and revision overflow; repository errors, duplicate/conflicting event IDs, transition-ID conflicts and stale snapshots; publication only after confirmed persistence; and a publish failure after a successful commit. The fake verifies the logical port contract only: it does not prove that a real adapter atomically compares and advances the persisted source cursor together with snapshot/history/event. It also does not prove transaction durability, power-loss behavior, a production transition-ID generator, durable outbox/reliable delivery, ESP32/Arduino builds or hardware integration. F03 duplicate detection remains atomically enforced by the repository during commit.


## Operation timeout tests (SSM-F11)

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp src/core/event_processing.cpp src/core/operations.cpp tests/unit/test_operation_timeout.cpp -o test_operation_timeout
    ./test_operation_timeout

The timeout tests cover UNKNOWN defaults for timeout type/certainty (fail closed), pre-dispatch timeout (FAILED only with explicit NotDispatched), dispatch/operation/cancellation timeout after possible delivery (OUTCOME_UNKNOWN), terminal and already-unknown operations, late timeouts, invalid event/type/certainty, F03 duplicate/conflict, F04 stale/unordered sequence, operation/correlation lookup, repository failures and concurrent updates. The test fake checks the logical transaction contract only; it does not test real monotonic timers, timeout durations, persistence durability, reboot recovery, outbox delivery, target builds or hardware. An integrated compile/run result must be recorded separately once executed.


## Failure disposition lookup tests (SSM-F12)

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/failure_disposition.cpp tests/unit/test_failure_disposition.cpp -o test_failure_disposition
    ./test_failure_disposition

The tests cover all 41 failure IDs from the current `docs/09`, §6 matrix, plus empty, unknown, malformed, case-mismatched and whitespace-mismatched IDs. They also verify deterministic repeated lookup. The function returns the matrix's preliminary reaction verbatim; it does not promote it to a final safety policy, choose a station state, authorize equipment, or execute a physical response. GCC and Clang strict-warning host builds were run. No SafetyManager, target build or hardware response is covered.


## General event handling tests (SSM-F13)

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp src/core/event_processing.cpp src/core/event_handling.cpp tests/unit/test_event_handling.cpp -o test_event_handling
    ./test_event_handling

The F13 tests cover the ordered common pipeline: envelope validation, exact event classification, duplicate/conflict/repository-error handling, source/session/sequence freshness, and delegation to the profile processor. They verify that invalid, unknown, duplicate, stale and unordered events do not reach the processor; registered categories and non-VALID data-quality statuses are forwarded unchanged for event-specific handling without parsing payload; processor outcomes are mapped without treating errors or unknown enum values as success. The profile processor must repeat event-ID/cursor checks atomically with its own state change to close races; F13's initial F03/F04 checks are read-only and F13 does not advance the cursor. This unit test does not implement or verify profile-specific payload adapters, durable transaction coordination, hardware, or full-system integration.

## Global transition profile processor tests (SSM-F13)

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp src/core/event_processing.cpp src/core/state_transitions.cpp src/core/global_transition_processor.cpp tests/unit/test_global_transition_processor.cpp -o test_global_transition_processor
    ./test_global_transition_processor

The tests cover the three approved startup event/category pairs, unsupported pairs, non-VALID quality, missing/error snapshots, unavailable/error/unknown context results, discarding partial positive context, UNKNOWN recovery status, transition-ID provider failures, F05 rejection before ID generation, F10 commit result mapping, unchanged expected source cursor, and publication uncertainty after a committed transition. The separate `test_event_handling.cpp` suite also checks that COMMITTED_NOTIFICATION_NOT_CONFIRMED is preserved through the F13 result mapping. Repository fakes validate the transaction contract only; they do not prove durable storage, physical atomicity, outbox delivery, target builds or hardware.

## Operation event processor contract tests (F13 → F09 adapter)

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -Isrc src/core/operation_event_processor.cpp tests/contract/test_operation_event_processor_contract.cpp -o test_operation_event_processor_contract
    ./test_operation_event_processor_contract

This contract harness defines a stub for `handle_operation_update()` so it can test event/category gating, exact quality gating, opaque-payload decoder behavior, forwarding of the unchanged Event and SourceCursor, and F09-result mapping. It does not test the real F09 function, actual repository adapters, durable storage, target builds or hardware. The real F09 algorithm remains covered separately by `tests/unit/test_operation_update.cpp`.


## Recovery start tests (SSM-F14)

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/recovery.cpp tests/unit/test_recovery_start.cpp -o test_recovery_start
    ./test_recovery_start

The F14 tests use fake recovery and report-request ports. They cover snapshot loading, malformed/unknown state and revision linkage, persistence of the RECOVERY_IN_PROGRESS marker before report requests, repeated/idempotent entry, snapshot races, repository and report-request errors, unknown enum values, and prevention of downstream calls when prerequisites are not confirmed. The isolated host harness does not prove the real storage adapter, report transport, correlation of asynchronous subsystem reports, F15 reconciliation, target build or hardware behavior.

## Recovery reconciliation tests (SSM-F15)

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/recovery.cpp tests/unit/test_recovery_reconcile.cpp -o test_recovery_reconcile
    ./test_recovery_reconcile

The F15 tests cover historical snapshot validation, complete/incomplete report collections, missing and stale data, unsafe and unknown subsystem conditions, duplicate subsystem IDs, full matching of unfinished operation records to executor reports by operation_id/correlation_id, unknown/extra/duplicate/missing operation reports, operation outcomes, and the contradiction between a persisted STARTED lifecycle and a current VALID NOT_EXECUTED report. The function is read-only: EvidenceComplete is not permission to resume work or a decision about the next global station state. The host harness uses compatible test declarations and does not prove the real adapter's report freshness, authenticity, completeness, recovery-attempt correlation, repository durability, target build or hardware behavior.


## State snapshot read tests (SSM-F16)

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp src/core/event_processing.cpp src/core/state_transitions.cpp tests/unit/test_snapshot_read.cpp -o test_snapshot_read
    ./test_snapshot_read

The F16 tests use a fake read-only StationStateSnapshotReader. They cover the initial revision-zero snapshot, a snapshot after transitions, exactly one repository read per function call, missing snapshot, repository error, unknown state, inconsistent revision/transition_id, whitespace-only transition ID, unknown port status, FOUND without a snapshot value, and absence of a snapshot value for every non-AVAILABLE result. The tests verify the logical read contract only; they do not prove production storage consistency, lifetime management in a real adapter, power-loss behavior, target builds or hardware integration.


## Transition history read tests (SSM-F17)

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -Isrc src/core/event_validation.cpp src/core/event_processing.cpp src/core/state_transitions.cpp tests/unit/test_transition_history.cpp -o test_transition_history
    ./test_transition_history

The F17 tests use a fake read-only TransitionHistoryReader. They cover an empty complete selection, ordered records, inclusive revision/state filtering, invalid filters rejected before repository access, incomplete/error/unknown port statuses, invalid records (unknown/equal states, zero revision, empty IDs, malformed reason pairs, and preservation of a valid reason pair), unordered/duplicate revisions, duplicate event_id/transition_id values across records, and records outside the requested filter. Partial results are never returned as complete history. The isolated harness validates the logical algorithm only; it does not prove production storage completeness, retention policy, durable F10 transactions, target builds or hardware integration.
