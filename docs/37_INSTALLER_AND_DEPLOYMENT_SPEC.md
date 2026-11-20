# DockStation installers and deployment contract

**Status:** implementation specification for the first distributable software slice.  
**Date:** 2026-11-20.  
**Scope:** Windows Operator installer and native-architecture Linux Gateway Debian packages (amd64 for x86_64 hosts and arm64 for Repka).

## 1. Product boundaries

The two installers are independent products. Neither installer configures physical GPIO/UART wiring, creates device secrets, provisions ESP32 credentials, or performs a firmware flash automatically.

### Windows installer (.exe)

The Windows installer contains:

- the current QML DockStation Operator entry point;
- the Operator Python application and its QML resources;
- the Python runtime and PySide6/Qt runtime required by the application, bundled by the frozen application build;
- application shortcuts and an uninstaller.

It does not contain:

- Linux Gateway binaries or systemd units;
- ESP32 firmware or ESP-IDF;
- SSH private keys, passwords, known_hosts files, station profiles, or other user data;
- a copy of OpenSSH. Windows OpenSSH Client is an operating-system capability and must be present separately.

Target: Windows 10/11 x64. The installer must not require Python to be installed on the target PC. It must not silently enable/install Windows optional features or weaken SSH host-key checking. The initial CI artifact is not Authenticode-signed because no code-signing certificate has been configured; GitHub build provenance, if enabled, is not a substitute for Windows publisher signing and SmartScreen may show an unknown-publisher warning.

### Linux Gateway package (.deb)

The package is built natively for the target Debian architecture: amd64 for x86_64 Linux hosts and arm64 for Repka Pi 5. The same package payload and lifecycle policy are used for both architectures; the ELF machine type, Debian Architecture field, runtime dependencies and filename must match the actual native build. Cross-architecture packaging is not implied. It contains:

- dockstation_linux_bootstrap;
- dockstation-credential-provision;
- dockstation-registry-provision;
- dockstation-gateway.service and dockstation-gateway-uart@.service;
- the example Gateway configuration and the package's initial /etc/dockstation/gateway.conf.

It does not contain:

- CMake, a C++ compiler, development headers, or source code;
- Python Gateway/test-only components or Windows Operator;
- ESP-IDF or a compiler for ESP32;
- an automatically executed ESP32 flash/provisioning operation.

Runtime shared-library dependencies are derived from the actual native binaries at package-build time and written into Debian control metadata. For Repka, the exact target OS image and its package/ABI compatibility remain a release gate; arm64 alone does not prove compatibility with every Repka OS image. The amd64 package is intended for x86_64 Debian/Ubuntu hosts and is also used to validate the installer path on the development server.

## 2. Gateway service and configuration policy

- The package installs and enables only dockstation-gateway.service, the read-only base Gateway service.
- The base service retains PrivateDevices=true; it does not receive GPIO, UART, camera, or other hardware access.
- dockstation-gateway-uart@.service is installed but not enabled or started. No UART device is selected automatically.
- The initial configuration leaves all ControllerLink UART/identity settings commented.
- /etc/dockstation/gateway.conf is a Debian conffile. Package upgrades preserve local changes.
- The Gateway runs as the dedicated dockstation system user/group, without a login shell.
- Persistent state belongs under /var/lib/dockstation; package upgrades and removal do not delete it.
- The package does not store PSK/private credentials in the SQLite database, package metadata, or logs.

## 3. ESP32 firmware and provisioning boundary

Firmware is deliberately excluded from the first .deb until the physical board, Flash size, production partition table, and target profile are confirmed. The current CI firmware image inherits a 2 MB default partition profile while the documented candidate board likely has 4 MB; that CI image must not be flashed to the physical board.

The future package may carry a production firmware image and a dedicated dockstation-esp32-setup helper only after the image is built for an approved board/Flash/partition profile. The helper must require explicit invocation, run read-only device/profile preflight, validate image compatibility and credential readiness, and fail closed without erasing, flashing, generating keys, or burning eFuses when prerequisites are missing. Installing or upgrading .deb must never invoke it.

The current ESP32 credential store reads pre-provisioned encrypted NVS only. A separately reviewed provisioning workflow for NVS keys, PSK/identity and stable controller IDs is required before real ControllerLink activation.

## 4. Lifecycle scenarios

### Windows

| Scenario | Required behaviour |
|---|---|
| Fresh install | Install application under Program Files; create Start Menu shortcut; do not require system Python; do not create station profiles or SSH credentials. |
| Upgrade | Replace application binaries; preserve %LOCALAPPDATA%/DockStationOperator and any existing %APPDATA% fallback data. |
| Uninstall | Remove application binaries and shortcuts; preserve profiles, logs, SSH keys, known_hosts and other user data. Explain where the data remains. |
| Restore | Reinstall the same or newer installer; existing user data is reused. Recovery of lost SSH keys/known_hosts is the user's independent backup responsibility. |

### Linux Gateway package (amd64 / arm64)

| Scenario | Required behaviour |
|---|---|
| Fresh install | Verify that the package architecture matches the native host and runtime dependencies; install files; create the system account/group and initial config if absent; enable/start only the read-only base service; verify local health endpoint. |
| Upgrade | Preserve /etc/dockstation/gateway.conf and /var/lib/dockstation; stop/restart the base service through package lifecycle; do not alter UART selection or secrets. |
| Remove | Stop and disable the service; remove package-owned executables/units; retain configuration and persistent state for a later reinstall. |
| Purge | Remove package conffiles according to dpkg policy, but do not silently delete /var/lib/dockstation; data deletion is a separate explicit administrative action. |
| Restore | Reinstall package, restore configuration and database from an independently verified backup if needed, then verify service and /api/v1/health. The package does not claim to implement automatic backup/restore. |

## 5. Release gates and verification

A release is not ready merely because the package can be built.

- Windows: build the frozen QML application, compile the installer, verify the installer payload and run a packaged-app QML startup smoke test on Windows CI. Perform a manual install/upgrade/uninstall check on a supported Windows 10/11 machine before declaring a user-facing release.
- amd64: build the package on a native x86_64/amd64 runner, verify the ELF machine type and Debian metadata, inspect payload and dependencies, and exercise install/upgrade/remove/purge lifecycle in an isolated Ubuntu/Debian environment.
- Repka arm64: build the package on a native ARM64 runner, inspect package metadata and payload, install/upgrade/remove it in an isolated Debian-compatible ARM64 environment, verify conffile/state preservation, verify service lifecycle and health endpoint, and inspect dependency resolution.
- Before distributing to the actual Repka: record /etc/os-release, dpkg --print-architecture, libc, OpenSSL and SQLite runtime versions from the exact vendor image; confirm the package dependency/ABI floor against that image.
- Physical UART, ESP32, sensors, keys, pinmux and firmware are hardware acceptance items, not package CI claims.
