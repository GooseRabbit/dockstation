#define _XOPEN_SOURCE 600

#include "platform/linux/prototype_uart_telemetry_service.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool write_frame(int descriptor, const std::string& frame) {
    const ssize_t written = ::write(descriptor, frame.data(), frame.size());
    return written == static_cast<ssize_t>(frame.size());
}

}  // namespace

int main() {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (!check(master >= 0, "Could not create pseudo-terminal")) return 1;
    if (!check(::grantpt(master) == 0, "Could not grant pseudo-terminal")) {
        ::close(master);
        return 1;
    }
    if (!check(::unlockpt(master) == 0, "Could not unlock pseudo-terminal")) {
        ::close(master);
        return 1;
    }
    char* const slave_path = ::ptsname(master);
    if (!check(slave_path != nullptr, "Could not resolve pseudo-terminal path")) {
        ::close(master);
        return 1;
    }

    dockstation::platform::linux_platform::PrototypeUartTelemetryService service;
    if (!check(service.configure(slave_path), "Service rejected a valid /dev/pts path")) {
        ::close(master);
        return 1;
    }
    service.poll();  // Opens and configures the receive side at 115200 8N1.

    if (!check(write_frame(master, "DS1,7,12000,21.5,46.0,22.0,44.5\n"),
               "Could not write the valid telemetry frame")) {
        ::close(master);
        return 1;
    }
    service.poll();
    if (!check(service.has_snapshot(), "A valid telemetry frame was not accepted") ||
        !check(service.connected(), "A freshly received frame is not marked connected") ||
        !check(service.sequence_number() == 7U, "Sequence number was not retained") ||
        !check(service.measurement(0U).available &&
                   service.measurement(0U).value == 21.5,
               "Sensor 1 temperature was not decoded") ||
        !check(service.measurement(3U).available &&
                   service.measurement(3U).value == 44.5,
               "Sensor 2 humidity was not decoded")) {
        service.stop();
        ::close(master);
        return 1;
    }

    // The USB/UART0 console wraps the same frame in the ESP-IDF log prefix.
    if (!check(write_frame(master,
            "I (1450) DockStationESP32: DS1,8,14000,23.5,42.0,NA,NA\n"),
               "Could not write the ESP-IDF log-framed telemetry frame")) {
        service.stop();
        ::close(master);
        return 1;
    }
    service.poll();
    if (!check(service.sequence_number() == 8U,
               "ESP-IDF log-framed DS1 telemetry was not accepted") ||
        !check(service.measurement(0U).available &&
                   service.measurement(0U).value == 23.5,
               "USB/UART0 DHT22 temperature was not decoded") ||
        !check(!service.measurement(2U).available,
               "Missing second sensor was not retained as unavailable")) {
        service.stop();
        ::close(master);
        return 1;
    }

    // Invalid ranges must not replace the last accepted snapshot.
    if (!check(write_frame(master, "DS1,9,16000,21.0,101.0,22.0,44.0\n"),
               "Could not write the invalid telemetry frame")) {
        service.stop();
        ::close(master);
        return 1;
    }
    service.poll();
    if (!check(service.sequence_number() == 8U,
               "Out-of-range humidity replaced an accepted snapshot")) {
        service.stop();
        ::close(master);
        return 1;
    }

    // NA is an explicit unavailable reading, not a fabricated zero.
    if (!check(write_frame(master, "DS1,9,18000,NA,NA,22.2,44.2\n"),
               "Could not write the NA telemetry frame")) {
        service.stop();
        ::close(master);
        return 1;
    }
    service.poll();
    const bool na_handled = service.sequence_number() == 9U &&
        !service.measurement(0U).available &&
        !service.measurement(1U).available &&
        service.measurement(2U).available &&
        service.measurement(2U).value == 22.2;
    if (!check(na_handled, "NA fields were not represented as unavailable")) {
        service.stop();
        ::close(master);
        return 1;
    }

    // Unplugging the USB-UART bridge must invalidate the live link and close
    // the old tty descriptor while retaining the last snapshot as stale.
    const std::string original_slave_path{slave_path};
    ::close(master);
    service.poll();
    if (!check(!service.connected(), "TTY hangup did not mark telemetry disconnected") ||
        !check(service.has_snapshot(), "Last telemetry snapshot was not retained") ||
        !check(std::string{service.error_code()} == "UART_DEVICE_DISCONNECTED",
               "TTY hangup did not report a disconnected device")) {
        service.stop();
        return 1;
    }

    // Linux reuses the freed PTY number, modelling a USB device reappearing
    // under the same configured /dev path.
    const int reconnect_master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (!check(reconnect_master >= 0, "Could not create reconnect pseudo-terminal") ||
        !check(::grantpt(reconnect_master) == 0, "Could not grant reconnect pseudo-terminal") ||
        !check(::unlockpt(reconnect_master) == 0, "Could not unlock reconnect pseudo-terminal")) {
        if (reconnect_master >= 0) ::close(reconnect_master);
        service.stop();
        return 1;
    }
    char* const reconnect_slave_path = ::ptsname(reconnect_master);
    if (!check(reconnect_slave_path != nullptr, "Could not resolve reconnect pseudo-terminal") ||
        !check(reconnect_slave_path != nullptr &&
                   original_slave_path == reconnect_slave_path,
               "Reconnected pseudo-terminal did not reuse the configured device path")) {
        ::close(reconnect_master);
        service.stop();
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{2100});
    service.poll();  // Reopens the same configured path after the retry interval.
    if (!check(std::string{service.error_code()} == "UART_DEVICE_WAITING_FOR_FRAME",
               "Reopened UART did not wait for a fresh telemetry frame") ||
        !check(!service.connected(),
               "A reopened UART was marked connected before a fresh frame arrived")) {
        ::close(reconnect_master);
        service.stop();
        return 1;
    }

    if (!check(write_frame(
            reconnect_master, "DS1,1,2000,NA,NA,29.3,36.0\n"),
               "Could not write a frame after UART reconnection")) {
        ::close(reconnect_master);
        service.stop();
        return 1;
    }
    service.poll();
    const bool reconnected = service.connected() &&
        service.sequence_number() == 1U &&
        service.measurement(2U).available &&
        service.measurement(2U).value == 29.3 &&
        service.measurement(3U).available &&
        service.measurement(3U).value == 36.0;

    service.stop();
    ::close(reconnect_master);
    if (!check(reconnected, "UART did not resume telemetry after device reconnection")) {
        return 1;
    }
    return 0;
}
