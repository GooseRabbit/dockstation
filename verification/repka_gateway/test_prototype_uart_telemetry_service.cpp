#define _XOPEN_SOURCE 600

#include "platform/linux/prototype_uart_telemetry_service.hpp"

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
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

    // Invalid ranges must not replace the last accepted snapshot.
    if (!check(write_frame(master, "DS1,8,14000,21.0,101.0,22.0,44.0\n"),
               "Could not write the invalid telemetry frame")) {
        service.stop();
        ::close(master);
        return 1;
    }
    service.poll();
    if (!check(service.sequence_number() == 7U,
               "Out-of-range humidity replaced an accepted snapshot")) {
        service.stop();
        ::close(master);
        return 1;
    }

    // NA is an explicit unavailable reading, not a fabricated zero.
    if (!check(write_frame(master, "DS1,8,16000,NA,NA,22.2,44.2\n"),
               "Could not write the NA telemetry frame")) {
        service.stop();
        ::close(master);
        return 1;
    }
    service.poll();
    const bool na_handled = service.sequence_number() == 8U &&
        !service.measurement(0U).available &&
        !service.measurement(1U).available &&
        service.measurement(2U).available &&
        service.measurement(2U).value == 22.2;

    service.stop();
    ::close(master);
    if (!check(na_handled, "NA fields were not represented as unavailable")) return 1;
    return 0;
}
