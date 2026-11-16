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

An ESP-IDF component now exists at
\`components/controller_mbedtls_psk_server/\`. It implements the ESP32-side
TLS 1.3 server for the internal Repka link and receives encrypted UART bytes
through injected Mbed TLS send/receive callbacks. The fixed profile is
TLS 1.3 PSK-Ephemeral, TLS_AES_128_GCM_SHA256 and P-256; it checks the exact
16-byte pair identity before selecting the 32-byte PSK, blocks application
plaintext until handshake completion, disables early data and issues no
session tickets. Its 32-byte TLS exporter uses the same label/context as the
Repka OpenSSL client.

The component now also includes \`ControllerLinkPeerService\`. It starts the
shared logical peer session only after TLS authentication, sends HELLO_ACK,
blocks exporter access until HELLO_ACK has been transmitted, and turns an
accepted REQUEST_SNAPSHOT into a pending application request. The application
must supply a real typed TELEMETRY_SNAPSHOT; invalid sensor data is rejected
without consuming the request or closing the session. The transport can be
restarted on the same peer-session object while retaining sequence/replay
history for the current controller boot.

Host interop/integration tests connect the existing OpenSSL client to upstream
Mbed TLS 3.6.7 over a POSIX socket pair. They cover TLS handshake, exporter
parity, wrong PSK/identity rejection, bidirectional encrypted data,
HELLO/HELLO_ACK, exporter gating and REQUEST_SNAPSHOT/TELEMETRY_SNAPSHOT,
including invalid-snapshot retry. GitHub Actions
[run #43908199426](https://github.com/GooseRabbit/dockstation/actions/runs/43908199426)
passed both interop CTests. This is not yet an ESP-IDF firmware image and does
not prove compatibility with Espressif's Mbed TLS fork or physical UART.

The ESP-IDF application project, board profile and \`sdkconfig\` remain
intentionally uncreated until the exact ESP32 module/SoC and flash size are
confirmed. Secure NVS provisioning, Flash Encryption, Secure Boot, DHT22
acquisition and GPIO drivers remain separate tasks.
