#include "controller_uart_transport.hpp"

#include <climits>
#include <cstdint>
#include <limits>

#include "mbedtls/ssl.h"

namespace dockstation::firmware::esp32 {
namespace {

constexpr int kMaximumReceiveBufferSize = 64 * 1024;
constexpr int kUartEventQueueLength = 8;
constexpr std::size_t kMaximumEventsPerPoll = 8U;

[[nodiscard]] bool valid_config(const ControllerUartConfig& config) noexcept {
    const int port_number = static_cast<int>(config.port);
    if (port_number <= static_cast<int>(UART_NUM_0) ||
        port_number >= static_cast<int>(UART_NUM_MAX)) {
        return false;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(config.tx_gpio) ||
        !GPIO_IS_VALID_GPIO(config.rx_gpio) ||
        config.tx_gpio == config.rx_gpio) {
        return false;
    }
    if (config.baud_rate <= 0 || config.baud_rate > UART_BITRATE_MAX) {
        return false;
    }
    if (config.receive_buffer_size <= static_cast<int>(UART_HW_FIFO_LEN(config.port)) ||
        config.receive_buffer_size > kMaximumReceiveBufferSize) {
        return false;
    }
    return true;
}

}  // namespace

ControllerUartTransport::~ControllerUartTransport() {
    close();
}

bool ControllerUartTransport::is_valid_config(
    const ControllerUartConfig& config) noexcept {
    return valid_config(config);
}

bool ControllerUartTransport::open(const ControllerUartConfig& config) noexcept {
    close();
    if (driver_installed_) {
        // Do not attempt to reuse a UART whose previous driver failed to close.
        return false;
    }
    if (!is_valid_config(config)) {
        last_error_ = ESP_ERR_INVALID_ARG;
        return false;
    }

    uart_config_t uart_config{};
    uart_config.baud_rate = config.baud_rate;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.rx_flow_ctrl_thresh = 0U;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t result = uart_param_config(config.port, &uart_config);
    if (result != ESP_OK) {
        last_error_ = result;
        return false;
    }

    result = uart_set_pin(
        config.port,
        config.tx_gpio,
        config.rx_gpio,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);
    if (result != ESP_OK) {
        last_error_ = result;
        return false;
    }

    // The TX ring buffer is intentionally disabled: send_callback() uses
    // uart_tx_chars(), which copies only what currently fits in the HW FIFO.
    // Receive uses the driver's RX ring buffer with a zero-tick read timeout.
    result = uart_driver_install(
        config.port,
        config.receive_buffer_size,
        0,
        kUartEventQueueLength,
        &event_queue_,
        0);
    if (result != ESP_OK) {
        event_queue_ = nullptr;
        last_error_ = result;
        return false;
    }

    port_ = config.port;
    driver_installed_ = true;
    last_error_ = ESP_OK;
    return true;
}

void ControllerUartTransport::close() noexcept {
    if (driver_installed_) {
        const esp_err_t result = uart_driver_delete(port_);
        if (result != ESP_OK) {
            // Retain ownership state so a later close() can retry; never
            // pretend the callback's transport context is already gone.
            last_error_ = result;
            return;
        }
    }
    driver_installed_ = false;
    event_queue_ = nullptr;
    port_ = UART_NUM_MAX;
    last_error_ = ESP_OK;
}

bool ControllerUartTransport::poll_transport_events() noexcept {
    if (!driver_installed_ || event_queue_ == nullptr) {
        last_error_ = ESP_ERR_INVALID_STATE;
        return false;
    }

    for (std::size_t count = 0U; count < kMaximumEventsPerPoll; ++count) {
        uart_event_t event{};
        if (xQueueReceive(event_queue_, &event, 0) != pdTRUE) {
            return true;
        }

        switch (event.type) {
            case UART_FIFO_OVF:
            case UART_BUFFER_FULL:
            case UART_FRAME_ERR:
            case UART_PARITY_ERR:
            case UART_BREAK:
                // A byte may have been lost or corrupted. Do not continue
                // the authenticated stream and risk parsing a damaged record.
                last_error_ = ESP_ERR_INVALID_RESPONSE;
                return false;
            default:
                // UART_DATA and informational events leave bytes in the RX
                // ring buffer; receive_callback() drains them without waiting.
                break;
        }
    }

    // Work is bounded per runtime poll. Any remaining events are checked
    // before the next TLS/ControllerLink event-loop step.
    return true;
}

int ControllerUartTransport::send_callback(
    void* context, const unsigned char* buffer, std::size_t length) noexcept {
    if (context == nullptr) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    auto* transport = static_cast<ControllerUartTransport*>(context);
    if (!transport->is_open()) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    if (length == 0U) {
        return 0;
    }
    if (buffer == nullptr || length > std::numeric_limits<std::uint32_t>::max()) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }

    const int written = uart_tx_chars(
        transport->port_,
        reinterpret_cast<const char*>(buffer),
        static_cast<std::uint32_t>(length));
    if (written > 0) {
        return written;
    }
    if (written == 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }

    transport->last_error_ = ESP_FAIL;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

int ControllerUartTransport::receive_callback(
    void* context, unsigned char* buffer, std::size_t length) noexcept {
    if (context == nullptr) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    auto* transport = static_cast<ControllerUartTransport*>(context);
    if (!transport->is_open()) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    if (length == 0U) {
        return 0;
    }
    if (buffer == nullptr || length > static_cast<std::size_t>(INT_MAX) ||
        length > std::numeric_limits<std::uint32_t>::max()) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }

    const int received = uart_read_bytes(
        transport->port_,
        buffer,
        static_cast<std::uint32_t>(length),
        0);
    if (received > 0) {
        return received;
    }
    if (received == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    transport->last_error_ = ESP_FAIL;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

}  // namespace dockstation::firmware::esp32
