#pragma once

// Minimal ESP-IDF implementation of the Arduino calls used by Adafruit DHT.
// This intentionally does not pull in or initialize the Arduino Core.
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

typedef uint16_t word;

constexpr uint8_t INPUT = 0x01U;
constexpr uint8_t OUTPUT = 0x03U;
constexpr uint8_t INPUT_PULLUP = 0x05U;
constexpr uint8_t LOW = 0U;
constexpr uint8_t HIGH = 1U;

inline portMUX_TYPE& dht_timing_mux() noexcept {
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    return mux;
}

inline void noInterrupts() noexcept {
    portENTER_CRITICAL(&dht_timing_mux());
}

inline void interrupts() noexcept {
    portEXIT_CRITICAL(&dht_timing_mux());
}

inline void pinMode(uint8_t pin, uint8_t mode) noexcept {
    const auto gpio = static_cast<gpio_num_t>(pin);
    if (mode == INPUT_PULLUP) {
        (void)gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
        (void)gpio_set_direction(gpio, GPIO_MODE_INPUT);
    } else if (mode == INPUT) {
        (void)gpio_set_pull_mode(gpio, GPIO_FLOATING);
        (void)gpio_set_direction(gpio, GPIO_MODE_INPUT);
    } else if (mode == OUTPUT) {
        (void)gpio_set_pull_mode(gpio, GPIO_FLOATING);
        (void)gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    }
}

inline void digitalWrite(uint8_t pin, uint8_t value) noexcept {
    (void)gpio_set_level(static_cast<gpio_num_t>(pin), value == LOW ? 0 : 1);
}

inline int digitalRead(uint8_t pin) noexcept {
    return gpio_get_level(static_cast<gpio_num_t>(pin));
}

inline void delayMicroseconds(uint32_t microseconds) noexcept {
    esp_rom_delay_us(microseconds);
}

inline void delay(unsigned long milliseconds) noexcept {
    if (milliseconds == 0UL) return;

    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    if (ticks != 0U) {
        // One extra tick guarantees at least the requested wall-clock delay,
        // even when the call happens just before the next FreeRTOS tick.
        vTaskDelay(ticks + 1U);
        return;
    }

    esp_rom_delay_us(static_cast<uint32_t>(milliseconds * 1000UL));
}

inline uint32_t millis() noexcept {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000LL);
}
