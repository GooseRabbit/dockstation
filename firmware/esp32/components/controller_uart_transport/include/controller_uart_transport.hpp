#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace dockstation::firmware::esp32 {

// UART2 is the initial logical port candidate from DEC-062. GPIOs are
// deliberately left unset until the exact development-board module is checked.
struct ControllerUartConfig final {
    uart_port_t port{UART_NUM_2};
    gpio_num_t tx_gpio{GPIO_NUM_NC};
    gpio_num_t rx_gpio{GPIO_NUM_NC};
    int baud_rate{115200};
    int receive_buffer_size{4096};
};

// Non-blocking ESP-IDF UART byte stream for Mbed TLS BIO callbacks.
// A single task owns this object and the Mbed TLS session using its callbacks.
class ControllerUartTransport final {
public:
    ControllerUartTransport() noexcept = default;
    ~ControllerUartTransport();

    ControllerUartTransport(const ControllerUartTransport&) = delete;
    ControllerUartTransport& operator=(const ControllerUartTransport&) = delete;
    ControllerUartTransport(ControllerUartTransport&&) = delete;
    ControllerUartTransport& operator=(ControllerUartTransport&&) = delete;

    // Opens raw 8N1 UART with no flow control. Explicit TX/RX GPIOs are
    // mandatory; UART0 is rejected so the application cannot steal the console.
    [[nodiscard]] bool open(const ControllerUartConfig& config) noexcept;
    void close() noexcept;

    // Drain a bounded number of driver events. RX overflow, frame/parity
    // errors and BREAK invalidate the protected byte stream.
    [[nodiscard]] bool poll_transport_events() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return driver_installed_; }
    [[nodiscard]] esp_err_t last_error() const noexcept { return last_error_; }
    [[nodiscard]] uart_port_t port() const noexcept { return port_; }

    // Compatible with mbedtls_ssl_send_t / mbedtls_ssl_recv_t callbacks.
    // Returns WANT_WRITE / WANT_READ instead of waiting for FIFO/data.
    [[nodiscard]] static int send_callback(
        void* context, const unsigned char* buffer, std::size_t length) noexcept;
    [[nodiscard]] static int receive_callback(
        void* context, unsigned char* buffer, std::size_t length) noexcept;

private:
    uart_port_t port_{UART_NUM_MAX};
    esp_err_t last_error_{ESP_OK};
    bool driver_installed_{false};
    QueueHandle_t event_queue_{nullptr};
};

}  // namespace dockstation::firmware::esp32
