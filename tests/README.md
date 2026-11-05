# Host unit tests

The first StationStateManager validation function is intentionally independent
of Arduino APIs and hardware drivers. The code is written against C++17 as a
provisional portable implementation; the final language/toolchain decision is
still open in docs/12_DECISIONS_AND_OPEN_QUESTIONS.md.

## Run with GCC or Clang

From the repository root:

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp tests/unit/test_event_validation.cpp -o test_event_validation
    ./test_event_validation

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/state_transitions.cpp tests/unit/test_state_transitions.cpp -o test_state_transitions
    ./test_state_transitions

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
