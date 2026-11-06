@echo off
setlocal
chcp 65001 >nul

rem Detect the standard MSYS2 UCRT64 installation even when PATH was not saved.
if exist "C:\msys64\ucrt64\bin\g++.exe" set "PATH=C:\msys64\ucrt64\bin;%PATH%"

where g++ >nul 2>nul
if errorlevel 1 (
    echo [ERROR] GCC C++ compiler g++ was not found.
    echo Install the MSYS2 UCRT64 toolchain or add its bin folder to PATH.
    echo Typical folder: C:\msys64\ucrt64\bin
    pause
    exit /b 1
)

py -3 --version >nul 2>nul
if errorlevel 1 (
    python --version >nul 2>nul
    if errorlevel 1 (
        echo [ERROR] Python 3 was not found.
        echo Install Python 3 with Tkinter enabled.
        pause
        exit /b 1
    )
    set "PYTHON_CMD=python"
) else (
    set "PYTHON_CMD=py -3"
)

rem The testbench folder is tools\station_testbench; move to repository root.
pushd "%~dp0\..\.."
if errorlevel 1 (
    echo [ERROR] Could not locate the DockStation repository root.
    pause
    exit /b 1
)

if not exist "src\core\state_transitions.cpp" (
    echo [ERROR] src\core\state_transitions.cpp was not found.
    echo Check that this file is inside the repository's src\core folder.
    popd
    pause
    exit /b 1
)

if not exist "tools\station_testbench\testbench_engine.cpp" (
    echo [ERROR] Testbench C++ source was not found.
    echo Extract the complete tools\station_testbench folder into the repository root.
    popd
    pause
    exit /b 1
)

echo [BUILD] Compiling the real StationStateManager functions F01-F05, F10, F16 and F17...
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Isrc ^
  src/core/event_validation.cpp ^
  src/core/event_processing.cpp ^
  src/core/state_transitions.cpp ^
  tools/station_testbench/testbench_engine.cpp ^
  -o tools/station_testbench/testbench_engine.exe
if errorlevel 1 (
    echo.
    echo [ERROR] C++ build failed. The testbench window was not started.
    popd
    pause
    exit /b 1
)

echo [OK] C++ engine built successfully.
echo [START] Opening the graphical testbench...
%PYTHON_CMD% tools\station_testbench\testbench_ui.py
set "APP_EXIT=%ERRORLEVEL%"
popd
if not "%APP_EXIT%"=="0" (
    echo [ERROR] Testbench closed with code %APP_EXIT%.
    pause
)
exit /b %APP_EXIT%
