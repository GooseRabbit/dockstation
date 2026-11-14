# DockStation ESP32 firmware

This directory is the dedicated home for the ESP32 application and its ESP-IDF
project. ESP32-specific startup code, board configuration, peripheral drivers,
firmware build configuration and flash/partition settings belong here.

## Boundaries

- The root `CMakeLists.txt` continues to build the Linux Gateway and host tests.
  It must not include ESP-IDF project configuration.
- ESP32 firmware will be built as a separate ESP-IDF project from this directory.
- Shared, transport-independent protocol code remains in
  `src/hardware/interfaces/`. The ESP-IDF project will consume that code through
  a small component wrapper; source files must not be copied into this folder.
- Linux Gateway, Linux serial-port code and Linux-only libraries must never be
  dependencies of the ESP32 firmware target.
- Host tests may exercise shared protocol code, but they are not ESP32 hardware
  simulation and are not presented as physical-device verification.

## Project status

This directory is reserved and its separation boundary is established. The
ESP-IDF project files, `main/` application component, board profile and
`sdkconfig` are intentionally not created yet: the exact ESP32 module/SoC,
flash size and final GPIO assignment have not been confirmed. Creating a
buildable target with guessed board settings would give a false sense of
hardware compatibility.

Once the board profile is confirmed, the ESP-IDF project will be added here
with its own `CMakeLists.txt`, `main/`, shared protocol component and explicit
build instructions. It will be built independently from the root Linux
Gateway build, without an ESP32 emulator or virtual GPIO/UART devices.
