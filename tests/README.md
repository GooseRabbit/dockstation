# Host unit tests

The first StationStateManager validation function is intentionally independent
of Arduino APIs and hardware drivers. The code is written against C++17 as a
provisional portable implementation; the final language/toolchain decision is
still open in docs/12_DECISIONS_AND_OPEN_QUESTIONS.md.

## Run with GCC or Clang

From the repository root:

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc src/core/event_validation.cpp tests/unit/test_event_validation.cpp -o test_event_validation
    ./test_event_validation

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
