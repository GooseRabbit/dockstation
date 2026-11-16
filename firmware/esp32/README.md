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

The ESP-IDF target build including the stable controller-ID NVS reader passed
on commit `8219cd5023a881bd66139a4365b5fd3fbdb43ea3`:
[ESP32 firmware build #43939967553](https://github.com/GooseRabbit/dockstation/actions/runs/43939967553).
The log confirms that the NVS credential/identity component compiled and
linked, and the ESP32 ELF/BIN were generated. This is compile/link acceptance
only; the workflow does not flash or run the image on a physical board.

## Board identification and limitations

The owner supplied FCC ID **`2A54N-ESP32`**. The FCC filing identifies an
**ESP32 Development Board** from Shenzhen HiLetgo E-Commerce Co., Ltd. The
user manual attached to the same FCC application identifies an ESP-WROOM-32
module and shows a 38-pin board layout. Its pin definition lists GPIO17 as
TX2/U2TXD on board pin 28 and GPIO16 as RX2/U2RXD on board pin 27.
Espressif's official ESP32-WROOM-32 datasheet specifies 4 MB integrated SPI
Flash for the standard module.

This is a documented **UART2 candidate**, not a physically verified
profile. The FCC filing/manual do not prove that the particular board in the
owner's possession has the same PCB revision, module suffix, Flash/PSRAM
capacity or unmodified header routing. GPIO17/GPIO16 therefore remain
`GPIO_NUM_NC` in `ControllerUartConfig`; neither pin is assigned at startup.
The UART0 pins GPIO1/TX0 and GPIO3/RX0 remain reserved for the board's USB
serial/console path and are not selected for the station link.

`IDF_TARGET=esp32` remains a provisional family-level build target. The
physical module suffix/revision, actual Flash chip identity, PSRAM presence,
exact board revision and production partition layout remain unconfirmed.
4 MB is expected only if the installed module is verified as standard
ESP32-WROOM-32; it is not yet a measured property of the board. See
`docs/33_ESP32_HARDWARE_PROFILE.md` for the evidence matrix and acceptance
steps.
## Current firmware entry point

`main/app_main.cpp` remains diagnostic-only. It generates the per-boot
instance ID, evaluates startup prerequisites and logs every blocker. It does
not initialize NVS, install the UART driver, initialize TLS or call runtime
`start()`. The main component depends on the existing ESP32-side ControllerLink
components so the ESP-IDF build compiles the complete target path.

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

`controller_link_firmware_runtime/` composes the UART adapter and
`ControllerLinkPeerService`. Its `start()` accepts a
`ControllerLinkFirmwareRuntimeStartConfig`, caller-loaded pair credentials,
and an already-generated `ControllerInstanceIdProvider`. Before opening UART,
it requires an explicit `hardware_profile_confirmed` flag, a valid non-console
UART port with explicit distinct TX/RX GPIOs, nonzero PSK and PSK identity,
a generated boot instance ID, a nonzero major protocol version, and distinct valid local/peer controller IDs.
A caller-supplied local instance ID must be empty or match the generated UUIDv4.
The peer-session object copies that ID into bounded owned storage.
`restart_transport()` reuses the same session, boot ID and sequence/replay
history. The runtime does not load or generate PSK credentials and does not
acquire sensor data.

The pure `ControllerLinkStartupReadiness` evaluator exposes missing
startup prerequisites as explicit blockers, including a zero major protocol version and conflicting boot ID, and is covered by host CTest.
`app_main` evaluates the default-off configuration and logs the blockers.
The current configuration deliberately has no confirmed board profile, no
assigned UART GPIOs, no loaded pair credentials and no stable local/peer
controller IDs. The NVS reader is not called during this bootstrap.

The `controller_instance_identity/` component generates a fresh UUIDv4
`controller_instance_id` once per ESP32 boot. It calls
`bootloader_random_enable()`, fills 16 bytes with `esp_fill_random()`, and
immediately calls `bootloader_random_disable()`. The call is made early in
`app_main`, before ADC, I²S, Wi-Fi or Bluetooth are initialized; the ESP-IDF
documentation warns against concurrent use of the entropy source with RF/ADC.
The formatted 36-character value lives only in static RAM and is not logged or
persisted. It is distinct from Station ID, stable `controller_id` and PSK
identity. The runtime API requires this provider for every initial start;
transport reconnects retain the same value.

A separate `controller_psk_nvs_credential_store/` component provides a
read-only PSK/identity loader. It requires runtime Flash Encryption to be
already enabled, an encrypted `nvs_keys` partition, and the `nvs` data
partition. It reads existing NVS encryption keys and explicitly initializes
NVS with them; it never creates, repairs, writes, rotates or erases keys. The
credential namespace is `dockstation` with binary blobs `pair_psk`
(32 bytes) and `pair_identity` (16 bytes), plus NVS strings `controller_id`
(this ESP32's stable ID) and `peer_ctrl_id` (the paired Repka ID).
Both IDs must be distinct, non-empty ASCII identifiers of at most 128 bytes
using only letters, digits, `_`, `-`, `.` or `:`. The read-only loader
rejects missing, malformed or identical values and clears its output on
failure. It never creates or repairs records. Provisioning these two IDs and
the pair secret, and creating the production partition table, remain separate tasks.

Not implemented by this scaffold:

- board-specific UART pin profile or a running UART connection;
- GPIO, lid limit switches, motor control or interlocks;
- DHT22 acquisition or any other sensor driver;
- PSK provisioning/writing/rotation; the read-only NVS provider requires an existing production-encrypted key partition;
- enabling/configuring Flash Encryption, Secure Boot policy or OTA update;
- production partition/Flash sizing and final board profile.

The CI build currently inherits ESP-IDF v5.5.6's default 2 MB SPI-flash
selection because the board's actual Flash capacity has not been confirmed.
This is only the SDK build default, not evidence that the physical board has
2 MB. The 4 MB value in the standard ESP32-WROOM-32 datasheet is a module
specification, but it becomes applicable to this particular board only after
the actual module is confirmed and `flash_id` is read.
The CI workflow only builds and never flashes the hardware. Do not flash or
distribute this image for use on the board until the module/Flash capacity and
partition layout are verified and configured.

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
