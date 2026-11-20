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

`main/app_main.cpp` generates a fresh per-boot instance ID before ADC, I²S or
radio subsystems are used. It then applies two guards **before touching NVS**:
the exact hardware profile must be explicitly confirmed and the UART port/TX/RX
configuration must be valid. The checked-in profile deliberately keeps
`hardware_profile_confirmed=false` and the GPIOs unset, so the current image
returns before opening NVS.

Only after those two guards have been deliberately satisfied will
`app_main` initialize the already-provisioned encrypted NVS store in
read-only-key mode, load the existing PSK/identity and stable local/peer
controller IDs, and run the common startup-readiness evaluator. It never
generates, repairs, writes, rotates or erases credentials. Any failed load
clears secret output and leaves ControllerLink NOT_CONFIGURED.

When preflight succeeds, `app_main` creates one dedicated FreeRTOS task.
That task exclusively owns `ControllerLinkFirmwareRuntime` and polls the
non-blocking UART/TLS/protocol state machine every 10 ms (or one scheduler tick
if the configured tick rate requires it). The task stays alive after a
transient failure: it closes TLS before UART, reloads pair credentials through
the already initialized read-only encrypted-NVS provider, and retries the
transport on the same peer-session object. The boot instance ID and sequence /
replay history therefore survive reconnects. Retry delays are 1, 2, 4, 8, 16
and then 30 seconds, capped at 30 seconds; a fully established logical session
resets the delay to one second. TLS plus HELLO handshake has a three-second
deadline, and an established peer must provide a valid message within five
seconds. The Repka-side Linux transport independently applies a ten-second
monotonic deadline to its initial TLS handshake; expiry tears down both UART
and TLS state so the link service can enter its normal reconnect backoff.

An accepted `REQUEST_SNAPSHOT` is answered with eight logical station-state
measurements: lid position, platform/cleaning/drive activity, safety-stop state,
workspace state, last command decision and pending-command state. Unavailable
physical inputs are represented as `UNKNOWN` with `Unknown` quality; an
unconfigured lid is `NOT_CONFIGURED`. The runtime acquires two DHT22 channels independently of ControllerLink:
sensor 1 uses GPIO25 and sensor 2 uses GPIO26. The GPIO reader, bounded polling,
decoder and telemetry path are connected in firmware. Physical wiring,
electrical levels, pull-ups and actual sensor readings still require acceptance
on the board. No motor state or limit-switch value is fabricated; the lid
tracker remains a pure component awaiting GPIO acquisition. The temporary envelope
time marker is `mono_us_<value>`, monotonic microseconds since boot; it is
**not UTC wall-clock time**, and the final timestamp convention remains open.

The checked-in ControllerLink profile still has
`hardware_profile_confirmed=false` and unset UART GPIOs. The normal image
therefore skips NVS, UART and TLS setup, but it still creates the runtime task
for independent DHT22 acquisition on GPIO25/GPIO26. ControllerLink remains
NOT_CONFIGURED. The link-only acceptance hook is forced into the ELF for
compile/link coverage but is not executed.

The Mbed TLS server reseeds its per-connection DRBG when TLS is
reinitialized. On the current ESP32 scaffold this briefly enables the SAR-ADC
entropy source. Do not add concurrent ADC, I2S or RF users without coordinating
that entropy-source window; current DHT22/limit-switch scaffolding does not use
those peripherals.

The default Mbed TLS configuration enables the required TLS 1.3 server,
PSK-Ephemeral key exchange, AES-128-GCM/SHA-256, P-256 and keying-material
export. TLS 1.2 and server session tickets are disabled in this initial
profile. These compile-time settings do **not** mean that a live TLS session is
started.

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
It remains the final fail-closed policy check after the gated NVS reads. The
checked-in configuration does not reach this stage because the physical
hardware-profile guard remains false.

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
NVS with them; it never creates or repairs encryption keys. Credential and
controller-ID values are read with `NVS_READONLY` handles. The credential
namespace is `dockstation` with binary blobs `pair_psk` (32 bytes) and
`pair_identity` (16 bytes), plus NVS strings `controller_id` (this ESP32's
stable ID) and `peer_ctrl_id` (the paired Repka ID). Both IDs must be
distinct, non-empty ASCII identifiers of at most 128 bytes using only letters,
digits, `_`, `-`, `.` or `:`. The loader rejects missing, malformed or
identical values and clears its output on failure. It never creates or repairs
records.

## DHT22 / AM2302 frame decoder

`components/controller_dht22_sensor/` contains a pure five-byte frame decoder.
It validates the exact frame length and checksum,
decodes humidity and temperature in tenths, handles the temperature sign bit,
and rejects values outside 0–99.9 %RH and −40.0…+80.0 °C. The output is
cleared on every failure. Keeping integer tenths avoids premature rounding.

The component also contains a pure `Dht22TelemetryAdapter`. It maps one
decoded reading to two `ControllerTelemetryMeasurement` values: temperature
in `°C` and relative humidity in `%RH`. The caller supplies both stable
sensor IDs, the non-empty `measured_at` string and the quality status. The
adapter validates the IDs, quality enum and decoded value ranges, clears both
output entries on failure, and does not choose identifiers, invent timestamps
or infer freshness. The timestamp format remains the existing open protocol
decision. Returned string views refer to caller-owned storage, which must stay
valid while the measurements are used.

The component now also contains an ESP-IDF GPIO acquisition adapter. It drives the configured open-drain data pin low for 1.1 ms, releases it, captures the 40 data-bit high pulses with bounded edge waits, and passes the resulting five-byte frame through the existing checksum/range decoder. The reader returns explicit NOT_CONFIGURED, GPIO, timeout or invalid-frame outcomes; it does not return fabricated values. Use an external 4.7–10 kOhm data-line pull-up. The capture is polling-based and may reject a frame if an interrupt stretches a pulse; a later hardware timing review can replace this small adapter without changing the decoder or telemetry contract.

The firmware defines two independent DHT22 channels: sensor 1 DATA is GPIO25 and sensor 2 DATA is GPIO26. Each channel has its own sampling scheduler and cached real reading. These are the selected prototype assignments; verify the physical header labels and 3.3 V electrical compatibility before wiring. Sampling attempts are rate-limited independently, and snapshot telemetry reports NOT_CONFIGURED or UNAVAILABLE explicitly until a valid reading exists. Valid values are sent with their acquisition timestamp; values older than five seconds are marked STALE. The existing station-state measurements remain in the same snapshot.

The component also contains a pure `Dht22SamplingScheduler`. Each
physical sensor uses its own scheduler instance. The caller initializes it
with the monotonic timestamp when sensor power was applied and calls
`try_begin_read()` immediately before each acquisition attempt. The first
read is blocked until more than 2,000 ms after power-on; later attempts are
separated by at least 2,000 ms. A permitted attempt consumes its slot even if
the physical acquisition subsequently fails. A monotonic-clock regression
blocks further reads until explicit reinitialization.

The scheduler itself remains a pure policy component and does not read GPIO,
generate sensor timing, power a sensor, create a FreeRTOS task or run a timer.
The firmware runtime polling task owns both DHT22 channels and invokes the GPIO
reader only for explicitly configured pins. This task is started even when the
physical ControllerLink profile or protected credentials are unavailable; in
that mode it continues the independent sensor lifecycle without opening NVS,
UART or TLS. Both checked-in sensor pins remain unset, so the normal image
reports both channels as NOT_CONFIGURED. Sensor samples are not transmitted to Repka while ControllerLink is
NOT_CONFIGURED. The Repka-side ControllerLink receiver and diagnostics JSON
path are implemented and covered by a host PTY integration test for all four
DHT22 measurements. Physical UART exchange remains pending confirmation of
the hardware profile, GPIO mapping and provisioned credentials. The decoder, adapter and scheduler
are built by ESP-IDF and checked by the host CTest targets `dht22_frame_decoder`,
`dht22_telemetry_adapter` and `dht22_sampling_scheduler`; these tests do
not prove physical sensor operation or acquisition timing.

## Lid limit-switch state tracker

`components/controller_lid_limit_switches/` contains a pure, allocation-free
state tracker for the two lid end-stop inputs. The caller supplies each raw
electrical level (or `nullopt` when a sample could not be read), an explicit
active level for each configured switch, a non-zero debounce interval for each
input and a monotonic timestamp.

The tracker does not assume normally-open/normally-closed wiring, choose GPIO
pins, read GPIO or control the lid motor. A fresh or changing sample remains
`TRANSITIONING` until the full debounce interval has elapsed. Both endpoints
must be configured and stable to report `OPEN` or `CLOSED`. If neither
end-stop is active, the position is `TRANSITIONING` (not at a verified
endpoint); if both are active, an input is unavailable, the configuration is
invalid or monotonic time moves backwards, the result is `UNKNOWN`.
Two unconfigured endpoints produce `NOT_CONFIGURED`.

The host CTest target `lid_limit_switch_tracker` checks the state mapping,
mixed active polarities, debounce timing, contradictory signals, unavailable
samples, invalid/incomplete configuration, clock regression and reconfiguration.
These are software contract tests only; the electrical profile and physical
switch behaviour remain unverified.

The [Aosong AM2302 technical manual](https://aosong.com/uploadfiles/2025/04/20250417105409216.pdf)
defines the 40-bit frame, field order, scale, sign bit and checksum, and lists
the measurement range. It also specifies a minimum two-second interval between
reads and a wait of more than two seconds after power-up. These limits must be
respected by the future acquisition task.

Not implemented by this scaffold:

- a provisioning writer/utility for PSK, PSK identity or stable controller IDs;
- board-specific UART pin assignment or a running UART connection;
- physical acceptance of ControllerLink reconnect/recovery over the actual UART wiring;
- physical DHT22 acceptance on the actual board; the GPIO reader and telemetry path are implemented, but both sensor GPIOs remain unset and no real sensor has yet been tested;
- GPIO acquisition/pin assignment for the lid limit switches, motor control or hardware interlocks; the pure lid limit-switch state tracker exists but is not connected to hardware;
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

The server temporarily enables the ESP32 SAR-ADC entropy source
around `mbedtls_ctr_drbg_seed()` and disables it immediately afterward. This
is required because ESP-IDF documents that RNG output is only pseudo-random
when neither RF nor the SAR-ADC entropy source is active. Initialization and
transport restart must therefore run when no ADC, I2S or RF user is accessing
those subsystems; the firmware must not seed TLS DRBG concurrently with them.
This restriction is separate from the early per-boot UUID generation, which
has its own short entropy-source window.

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
exporter gating and REQUEST_SNAPSHOT/TELEMETRY_SNAPSHOT. In this host build,
ESP-IDF-specific entropy calls are excluded and Mbed TLS uses its platform
entropy source. This host acceptance is separate from the Espressif-fork
ESP-IDF target build and from physical UART/board acceptance.

The current CI sdkconfig does not enable Flash Encryption and the current
default partition table has no production-provisioned `nvs_keys` partition.
Thus the gated read-only provider cannot load real pair credentials in the
current unprovisioned build. Do not enable security eFuses, generate NVS keys
or treat the CI image as a provisioning image.


## Firmware size and runtime memory diagnostics

The ESP32 GitHub Actions job runs `idf.py size` and
`idf.py size-components` immediately after the successful build. The CI log
therefore records the linked image's Flash/section usage and the component
breakdown for each firmware build. These are static build measurements; they
do not measure runtime heap use.

After the physical hardware profile and credentials are deliberately enabled,
the ControllerLink FreeRTOS task logs runtime memory diagnostics at task start
and every 60 seconds:

- free internal 8-bit-capable heap;
- largest contiguous internal 8-bit-capable heap block;
- minimum-ever free internal heap since boot;
- current task stack high-water mark (ESP-IDF reports bytes).

The periodic readings are intended to capture the steady-state and reconnect
behaviour on the actual board. They do not yet constitute a measured TLS
handshake peak on hardware; compare the minimum-free-heap value before, during
and after authenticated connection attempts.

UART event handling is bounded to eight queued events per runtime poll. If
the queue still contains events after that bounded drain, the runtime defers
the TLS/ControllerLink step until a later poll has inspected the remaining
events. This prevents TLS from consuming bytes while older UART overflow or
framing/parity events are still waiting in the event queue.


UART diagnostics preserve the concrete failure class: FIFO overflow, RX buffer
full, frame error, parity error, BREAK, driver configuration/install/close
failure, and TX/RX callback failure. The runtime carries this class across
TLS/UART teardown and includes it beside the ESP-IDF error name in reconnect
logs.


## ControllerLink fault-injection verification

Host CTest exercises partial TLS application writes, peer loss during an
incomplete frame, reconnect with a fresh TLS and HELLO/HELLO_ACK exchange,
malformed lengths and CRC recovery, exact command replay, a second command
while one is pending, bounded UART event batches, deferred queue backlog,
FIFO overflow and UART framing faults.

The event-queue policy is shared by the ESP-IDF transport and a host test. The
host test validates bounded draining and fault classification; it does not
claim to exercise the physical ESP-IDF queue or electrical UART.

Repka does not automatically retransmit command requests. A repeated frame
with the same protocol sequence is rejected by replay protection, and a new
command while another is pending is rejected. If the connection is lost after
a command may have been accepted, its physical outcome remains unknown. The
current prototype has no durable command IDs or exactly-once execution across
controller reboots. No test simulates physical actuator completion.


## Station command policy and logical state

`controller_station_logic/` is the ESP32-side authorization/state boundary.
Normal motion commands require an explicit inactive safety-stop signal and
explicit feedback for platform, cleaning and drive motion. Missing feedback is
not treated as an idle mechanism. Lid commands additionally require a
confirmed debounced endpoint. Cleaning additionally requires a known clear
workspace. Stop requests remain admissible with missing feedback or an active
safety stop, but `ACCEPTED` means only that the request passed policy and is
pending; no motor is driven by this component.

Only one normal command may remain pending. A later normal command is rejected
without erasing the earlier pending request; a stop request may supersede it.
The pending state is cleared on connection loss or by the future actuator layer
only after physical completion feedback. There is currently no actuator layer,
GPIO acquisition, or physical completion acknowledgement in the firmware.

The polling task answers snapshot requests with logical station-state
telemetry plus the two DHT22 channels. Its unknown input qualities are
intentional. The DHT22 GPIO reader and telemetry path are implemented, but
both sensor pins remain unset until the actual wiring is confirmed. Lid
limit-switch debounce is still not connected to GPIO, and the actual board
profile remains default-off until physical acceptance.


## Safety-input freshness

The station-state layer now tracks the monotonic sample time separately for lid
position, the complete motion-feedback set, safety-stop feedback and workspace
confirmation. Motion/safety/workspace values are updated through
`set_safety_inputs`; lid position has its own update path so forwarding a
cached lid value cannot refresh its age. A value is usable only while its explicitly configured maximum
age has not elapsed. The boundary is inclusive: age equal to the configured
limit is still fresh; the first millisecond beyond it is stale. A zero limit
means that the input class is not trusted at all.

Freshness windows are supplied through
`ControllerStationInputFreshnessConfig`. They intentionally default to zero:
no polling rate, debounce interval or sensor response guarantee has yet been
validated on the physical prototype, so firmware must not invent a timeout.
Changing the freshness configuration invalidates prior samples and clears a
pending command. Each input source must report again under the new configuration.

Telemetry distinguishes never-received/unknown values (`UNKNOWN`,
`SensorQualityStatus::Unknown`) from expired values (`STALE`,
`SensorQualityStatus::Stale`). Normal commands are rejected when a previously
received safety-stop, motion, lid or workspace sample has expired. Stop
requests remain admissible. A new valid sample restores eligibility.

All timestamps are monotonic milliseconds from the same ESP32 boot clock; they
are not UTC and must never be compared with Repka or wall-clock timestamps.
GPIO acquisition is still not connected. This layer validates age only after
a caller supplies a sample; it does not prove that a caller actually sampled
the electrical input at that time. The hardware integration must timestamp
samples at acquisition, not when forwarding cached values.

 
## Prototype UART telemetry

A plaintext receive-only DHT22 telemetry path is available for isolated hardware prototyping. UART2 uses GPIO17 TX / GPIO16 RX at 115200 8N1; UART0 remains the USB console/programming port. The frame and Linux configuration are documented in docs/35_PROTOTYPE_UART_DHT22.md. This temporary transport does not provide authentication, encryption or commands.
