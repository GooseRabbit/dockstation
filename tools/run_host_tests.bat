@echo off
setlocal

rem Run all currently registered DockStation host tests from the repository root.
cd /d "%~dp0\.."

where g++ >nul 2>nul
if errorlevel 1 (
    echo [ERROR] g++ was not found in PATH.
    echo Install the MSYS2 UCRT64 MinGW-w64 GCC toolchain and add its bin folder to PATH.
    exit /b 1
)

set "BUILD_DIR=build\host-tests"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo.
echo === DockStation host test suite ===
echo Compiler: g++
echo Output: %BUILD_DIR%
echo.

call :run_default test_event_validation src\core\event_validation.cpp tests\unit\test_event_validation.cpp || goto :failed
call :run_default test_state_transitions src\core\event_validation.cpp src\core\event_processing.cpp src\core\state_transitions.cpp tests\unit\test_state_transitions.cpp || goto :failed
call :run_default test_event_freshness src\core\event_processing.cpp tests\unit\test_event_freshness.cpp || goto :failed
call :run_default test_event_classification src\core\event_processing.cpp tests\unit\test_event_classification.cpp || goto :failed
call :run_default test_event_duplicates src\core\event_processing.cpp tests\unit\test_event_duplicates.cpp || goto :failed
call :run_default test_action_request src\core\event_validation.cpp src\core\event_processing.cpp src\core\operations.cpp tests\unit\test_action_request.cpp || goto :failed
call :run_default test_operation_intent src\core\event_validation.cpp src\core\event_processing.cpp src\core\operations.cpp tests\unit\test_operation_intent.cpp || goto :failed
call :run_default test_operation_dispatch src\core\event_validation.cpp src\core\event_processing.cpp src\core\operations.cpp tests\unit\test_operation_dispatch.cpp || goto :failed
call :run_default test_operation_update src\core\event_validation.cpp src\core\event_processing.cpp src\core\operations.cpp tests\unit\test_operation_update.cpp || goto :failed
call :run_default test_transition_commit src\core\event_validation.cpp src\core\event_processing.cpp src\core\state_transitions.cpp tests\unit\test_transition_commit.cpp || goto :failed
call :run_default test_operation_timeout src\core\event_validation.cpp src\core\event_processing.cpp src\core\operations.cpp tests\unit\test_operation_timeout.cpp || goto :failed
call :run_strict test_operation_timeout_event_processor_contract src\core\operation_timeout_event_processor.cpp tests\contract\test_operation_timeout_event_processor_contract.cpp || goto :failed
call :run_default test_failure_disposition src\core\failure_disposition.cpp tests\unit\test_failure_disposition.cpp || goto :failed
call :run_default test_event_handling src\core\event_validation.cpp src\core\event_processing.cpp src\core\event_handling.cpp tests\unit\test_event_handling.cpp || goto :failed
call :run_strict test_event_processor_router_contract src\core\event_processor_router.cpp tests\contract\test_event_processor_router_contract.cpp || goto :failed
call :run_default test_global_transition_processor src\core\event_validation.cpp src\core\event_processing.cpp src\core\state_transitions.cpp src\core\global_transition_processor.cpp tests\unit\test_global_transition_processor.cpp || goto :failed
call :run_strict test_operation_event_processor_contract src\core\operation_event_processor.cpp tests\contract\test_operation_event_processor_contract.cpp || goto :failed
call :run_default test_recovery_start src\core\recovery.cpp tests\unit\test_recovery_start.cpp || goto :failed
call :run_default test_recovery_reconcile src\core\recovery.cpp tests\unit\test_recovery_reconcile.cpp || goto :failed
call :run_default test_snapshot_read src\core\event_validation.cpp src\core\event_processing.cpp src\core\state_transitions.cpp tests\unit\test_snapshot_read.cpp || goto :failed
call :run_strict test_transition_history src\core\event_validation.cpp src\core\event_processing.cpp src\core\state_transitions.cpp tests\unit\test_transition_history.cpp || goto :failed
call :run_strict test_station_event_pipeline_contract src\core\event_validation.cpp src\core\event_processing.cpp src\core\event_handling.cpp src\core\event_processor_router.cpp src\core\station_event_pipeline.cpp tests\contract\test_station_event_pipeline_contract.cpp || goto :failed
call :run_strict test_configuration_types tests\unit\test_configuration_types.cpp || goto :failed
call :run_strict test_configuration_schema_version src\configuration\configuration_manager.cpp tests\unit\test_configuration_schema_version.cpp || goto :failed
call :run_strict test_configuration_required_fields src\configuration\configuration_manager.cpp tests\unit\test_configuration_required_fields.cpp || goto :failed
call :run_strict test_configuration_values src\configuration\configuration_manager.cpp tests\unit\test_configuration_values.cpp || goto :failed
call :run_strict test_configuration_consistency src\configuration\configuration_consistency.cpp tests\unit\test_configuration_consistency.cpp || goto :failed
call :run_strict test_configuration_validation_contract src\configuration\configuration_validation.cpp tests\contract\test_configuration_validation_contract.cpp || goto :failed
call :run_strict test_configuration_snapshot src\configuration\configuration_snapshot.cpp tests\unit\test_configuration_snapshot.cpp || goto :failed
call :run_strict test_configuration_load_contract src\configuration\configuration_load.cpp tests\contract\test_configuration_load_contract.cpp || goto :failed

echo.
echo [SUCCESS] All 30 host test programs passed.
echo Note: host tests do not replace target builds, persistent-storage validation, or hardware tests.
exit /b 0

:run_default
set "TEST_NAME=%~1"
shift
set "TEST_SOURCES=%~1 %~2 %~3 %~4 %~5 %~6 %~7 %~8 %~9"
echo [BUILD] %TEST_NAME%
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc %TEST_SOURCES% -o "%BUILD_DIR%\%TEST_NAME%.exe"
if errorlevel 1 exit /b 1
"%BUILD_DIR%\%TEST_NAME%.exe"
if errorlevel 1 exit /b 1
echo [PASS] %TEST_NAME%
echo.
exit /b 0

:run_strict
set "TEST_NAME=%~1"
shift
set "TEST_SOURCES=%~1 %~2 %~3 %~4 %~5 %~6 %~7 %~8 %~9"
echo [BUILD] %TEST_NAME% (no exceptions / no RTTI)
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -Isrc %TEST_SOURCES% -o "%BUILD_DIR%\%TEST_NAME%.exe"
if errorlevel 1 exit /b 1
"%BUILD_DIR%\%TEST_NAME%.exe"
if errorlevel 1 exit /b 1
echo [PASS] %TEST_NAME%
echo.
exit /b 0

:failed
echo.
echo [FAILED] %TEST_NAME% did not build or did not pass.
echo Fix the first reported error before continuing.
exit /b 1
