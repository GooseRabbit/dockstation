#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD_DIR="build/host-tests"
mkdir -p "$BUILD_DIR"
CXX="${CXX:-g++}"
COMMON=(-std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc -Isrc/core -Isrc/configuration)

run_default() {
    local name="$1"
    shift
    echo
    echo "[BUILD] $name"
    "$CXX" "${COMMON[@]}" "$@" -o "$BUILD_DIR/$name"
    "$BUILD_DIR/$name"
    echo "[PASS] $name"
}

run_strict() {
    local name="$1"
    shift
    echo
    echo "[BUILD] $name (no exceptions / no RTTI)"
    "$CXX" "${COMMON[@]}" -fno-exceptions -fno-rtti "$@" -o "$BUILD_DIR/$name"
    "$BUILD_DIR/$name"
    echo "[PASS] $name"
}

run_default test_event_validation src/core/event_validation.cpp tests/unit/test_event_validation.cpp
run_default test_state_transitions src/core/event_validation.cpp src/core/event_processing.cpp src/core/state_transitions.cpp tests/unit/test_state_transitions.cpp
run_default test_event_freshness src/core/event_processing.cpp tests/unit/test_event_freshness.cpp
run_default test_event_classification src/core/event_processing.cpp tests/unit/test_event_classification.cpp
run_default test_event_duplicates src/core/event_processing.cpp tests/unit/test_event_duplicates.cpp
run_default test_action_request src/core/event_validation.cpp src/core/event_processing.cpp src/core/operations.cpp tests/unit/test_action_request.cpp
run_default test_operation_intent src/core/event_validation.cpp src/core/event_processing.cpp src/core/operations.cpp tests/unit/test_operation_intent.cpp
run_default test_operation_dispatch src/core/event_validation.cpp src/core/event_processing.cpp src/core/operations.cpp tests/unit/test_operation_dispatch.cpp
run_default test_operation_update src/core/event_validation.cpp src/core/event_processing.cpp src/core/operations.cpp tests/unit/test_operation_update.cpp
run_default test_transition_commit src/core/event_validation.cpp src/core/event_processing.cpp src/core/state_transitions.cpp tests/unit/test_transition_commit.cpp
run_default test_operation_timeout src/core/event_validation.cpp src/core/event_processing.cpp src/core/operations.cpp tests/unit/test_operation_timeout.cpp
run_strict test_operation_timeout_event_processor_contract src/core/operation_timeout_event_processor.cpp tests/contract/test_operation_timeout_event_processor_contract.cpp
run_default test_failure_disposition src/core/failure_disposition.cpp tests/unit/test_failure_disposition.cpp
run_default test_event_handling src/core/event_validation.cpp src/core/event_processing.cpp src/core/event_handling.cpp tests/unit/test_event_handling.cpp
run_strict test_event_processor_router_contract src/core/event_processor_router.cpp tests/contract/test_event_processor_router_contract.cpp
run_default test_global_transition_processor src/core/event_validation.cpp src/core/event_processing.cpp src/core/state_transitions.cpp src/core/global_transition_processor.cpp tests/unit/test_global_transition_processor.cpp
run_strict test_operation_event_processor_contract src/core/operation_event_processor.cpp tests/contract/test_operation_event_processor_contract.cpp
run_default test_recovery_start src/core/recovery.cpp tests/unit/test_recovery_start.cpp
run_default test_recovery_reconcile src/core/recovery.cpp tests/unit/test_recovery_reconcile.cpp
run_default test_snapshot_read src/core/event_validation.cpp src/core/event_processing.cpp src/core/state_transitions.cpp tests/unit/test_snapshot_read.cpp
run_strict test_transition_history src/core/event_validation.cpp src/core/event_processing.cpp src/core/state_transitions.cpp tests/unit/test_transition_history.cpp
run_strict test_station_event_pipeline_contract src/core/event_validation.cpp src/core/event_processing.cpp src/core/event_handling.cpp src/core/event_processor_router.cpp src/core/station_event_pipeline.cpp tests/contract/test_station_event_pipeline_contract.cpp
run_strict test_configuration_types tests/unit/test_configuration_types.cpp
run_strict test_configuration_schema_version src/configuration/configuration_manager.cpp tests/unit/test_configuration_schema_version.cpp
run_strict test_configuration_required_fields src/configuration/configuration_manager.cpp tests/unit/test_configuration_required_fields.cpp
run_strict test_configuration_values src/configuration/configuration_manager.cpp tests/unit/test_configuration_values.cpp
run_strict test_configuration_consistency src/configuration/configuration_consistency.cpp tests/unit/test_configuration_consistency.cpp
run_strict test_configuration_validation_contract src/configuration/configuration_validation.cpp tests/contract/test_configuration_validation_contract.cpp
run_strict test_configuration_snapshot src/configuration/configuration_snapshot.cpp tests/unit/test_configuration_snapshot.cpp
run_strict test_configuration_load_contract src/configuration/configuration_load.cpp tests/contract/test_configuration_load_contract.cpp
run_strict test_configuration_revision src/configuration/configuration_snapshot.cpp src/configuration/configuration_revision.cpp tests/unit/test_configuration_revision.cpp

echo
echo "[SUCCESS] All 31 host test programs passed."
