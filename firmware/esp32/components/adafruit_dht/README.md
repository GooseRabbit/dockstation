# Adafruit DHT Sensor Library

Upstream: https://github.com/adafruit/DHT-sensor-library
Pinned source release: 1.4.7
License: MIT (see license.txt).

DockStation builds the upstream DHT class as a native ESP-IDF component.
`DHT_ESP32_Compat.h` provides only the small GPIO, delay, timer and critical
section API used by Adafruit's implementation. Pulse durations are measured
with `esp_timer_get_time()`; Adafruit's frame, bit decoding, checksum and
DHT11/DHT22 value conversion remain in use.

The Arduino Core is not linked or initialized. In particular, this component
does not call `initArduino()` or initialize the default NVS partition, which
is reserved by DockStation's protected credential store.
