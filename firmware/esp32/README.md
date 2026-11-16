# DockStation ESP32 firmware

This directory is a standalone ESP-IDF project for the ESP32-side controller.
It is deliberately separate from the Linux Gateway build at the repository
root.

## Build profile

The first target-build profile is pinned to:

- ESP-IDF **v5.5.6**;
- Espressif's Mbed TLS fork **3.6.7**, bundled by that ESP-IDF release;
- provisional ESP-IDF target **`esp32`**.

ESP-IDF v5.5.6 is selected because the existing TLS server component is written
against and host-tested with the Mbed TLS 3.6 API. ESP-IDF v6.0.3 bundles the
Mbed TLS 4.x line, so moving to it requires a separate API-compatibility review
and target acceptance rather than an unverified SDK upgrade.

The project can be built from an ESP-IDF v5.5.6 environment:

```bash
cd firmware/esp32
idf.py set-target esp32
idf.py build
```

GitHub Actions runs the same target build in Espressif's official ESP-IDF
container using `.github/workflows/esp32-firmware.yml`.

The target build passed on commit `9708d31c9ee111c924b27ef120de42b74add3b73`:
[ESP32 firmware build #43919541466](https://github.com/GooseRabbit/dockstation/actions/runs/43919541466).
The log confirms that both `controller_mbedtls_psk_server` and
`controller_uart_transport` were compiled and linked into the ESP32 image.
This is compile/link acceptance only; the workflow does not flash or run the
image on a physical board.

## Board identification and limitations

The owner supplied FCC ID **`2A54N-ESP32`**. The FCC filing identifies the
product as an **ESP32 Development Board** from Shenzhen HiLetgo E-Commerce
Co., Ltd. It does not, by itself, establish the exact module/SoC revision,
Flash or PSRAM capacity, board revision, or exposed GPIO wiring.

Accordingly, `IDF_TARGET=esp32` is a provisional family-level build target,
not a confirmed bill-of-materials or production board profile. No Flash size,
partition layout, UART pin, GPIO assignment, or peripheral mapping is fixed
here. GPIO16/GPIO17 remain only a conditional UART2 candidate for a confirmed
classic ESP32-WROOM-32 module; they are not configured or used by this project.

## Current firmware entry point

`main/app_main.cpp` is intentionally diagnostic-only. It prints a startup
message and explicitly reports that hardware paths are not enabled. The main
component depends on `controller_mbedtls_psk_server` so the ESP-IDF build
compiles the existing ESP32-side ControllerLink implementation as part of the
target build.

The default Mbed TLS configuration enables the required TLS 1.3 server,
PSK-Ephemeral key exchange, AES-128-GCM/SHA-256, P-256 and keying-material
export. TLS 1.2 and server session tickets are disabled in this initial
profile. These compile-time settings do **not** mean that a live TLS session is
started: there is no credential loading or UART transport in `app_main`.

An ESP-IDF UART byte-stream adapter exists in
`components/controller_uart_transport/`. It provides non-blocking Mbed TLS
send/receive callbacks, uses the selected UART port with 8N1/no flow control,
and requires explicit TX/RX GPIOs. Its bounded event queue exposes RX FIFO and
ring-buffer overflow, framing/parity faults and BREAK; the firmware runtime
closes TLS before UART whenever the protected byte stream may be damaged.
UART2 is the logical candidate, but neither GPIO is assigned by this project.

The `controller_link_firmware_runtime/` component composes the UART adapter
and `ControllerLinkPeerService`: it owns startup, bounded polling, restart and
shutdown order. The runtime accepts credentials and session IDs from its
caller; it does not read or generate keys and does not acquire sensor data.
`app_main` still does not start it until board GPIO and secure credential
provisioning are defined.

A separate `controller_psk_nvs_credential_store/` component provides a
read-only PSK/identity loader. It requires runtime Flash Encryption to be
already enabled, an encrypted `nvs_keys` partition, and the `nvs` data
partition. It reads existing NVS encryption keys and explicitly initializes
NVS with them; it never creates, repairs, writes, rotates or erases keys. The
credential namespace is `dockstation` with exact binary blobs `pair_psk`
(32 bytes) and `pair_identity` (16 bytes). Missing partitions, unencrypted
key storage, absent records or wrong blob lengths fail closed and clear the
output credentials. This is a reader only: provisioning the pair secret and
creating the production partition table remain separate tasks.

Not implemented by this scaffold:

- board-specific UART pin profile or a running UART connection;
- GPIO, lid limit switches, motor control or interlocks;
- DHT22 acquisition or any other sensor driver;
- PSK provisioning/writing/rotation; the read-only NVS provider requires an existing production-encrypted key partition;
- enabling/configuring Flash Encryption, Secure Boot policy or OTA update;
- production partition/Flash sizing and final board profile.

The CI build currently inherits ESP-IDF v5.5.6's default 2 MB SPI-flash
selection because the board's actual Flash capacity has not been confirmed.
That default appears in the generated image/flash arguments; it is **not** a
claim about this board. The CI workflow only builds and never flashes the
hardware. Do not flash or distribute this image for use on the board until the
module/Flash capacity and partition layout are verified and configured.

## Architecture boundaries

- The root `CMakeLists.txt` continues to build the Linux Gateway and host
  verification; it does not include ESP-IDF project configuration.
- Shared, transport-independent ControllerLink code remains in
  `src/hardware/interfaces/`. The ESP-IDF component references those sources
  directly; they are not copied into the firmware directory.
- Linux Gateway, Linux serial-port code and Linux-only libraries are not
  dependencies of the ESP32 firmware target.
- Host tests and a successful ESP-IDF compile are software/build checks. They
  are not hardware simulation and do not prove operation on the physical board.

## Existing ControllerLink component

`components/controller_mbedtls_psk_server/` contains the ESP32-side TLS 1.3
server and `ControllerLinkPeerService`. The fixed profile is TLS 1.3
PSK-Ephemeral, TLS_AES_128_GCM_SHA256 and P-256; it checks the exact 16-byte
pair identity before selecting the 32-byte PSK, blocks application plaintext
until handshake completion, disables early data and issues no session tickets.
Its 32-byte TLS exporter uses the same label/context as the Repka OpenSSL
client.

The peer service starts the shared logical session only after TLS
authentication, sends HELLO_ACK, blocks exporter access until HELLO_ACK has
been transmitted, and turns an accepted REQUEST_SNAPSHOT into a pending
application request. The application must supply a real typed
TELEMETRY_SNAPSHOT; invalid sensor data is rejected without consuming the
request or closing the session. Transport restart preserves sequence/replay
history for the current controller boot.

Host interop/integration tests connect the OpenSSL client to upstream Mbed TLS
3.6.7 over a POSIX socket pair. They cover TLS handshake, exporter parity,
wrong PSK/identity rejection, bidirectional encrypted data, HELLO/HELLO_ACK,
exporter gating and REQUEST_SNAPSHOT/TELEMETRY_SNAPSHOT. This host acceptance
is separate from the Espressif-fork ESP-IDF target build and from physical
UART/board acceptance.

The credential reader is deliberately not invoked from `app_main`. The
current CI sdkconfig does not enable irreversible Flash Encryption and the
current default partition table has no production-provisioned `nvs_keys`
partition. Thus the read-only provider compiles but correctly cannot load a
real pair credential in the current unprovisioned build. Do not enable security
efuses or flash this image as a provisioning action.
