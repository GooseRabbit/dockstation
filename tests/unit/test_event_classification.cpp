#include <cstdlib>
#include <iostream>
#include <string_view>

#include "core/event_processing.hpp"

using namespace dockstation::core;

namespace {

int failures = 0;

// Создаёт минимальный конверт: classify_event() использует только event_type.
Event make_event(std::string_view event_type) {
    Event event{};
    event.event_id = "event-test";
    event.event_type = event_type;
    event.source = "test-source";
    event.received_at = "2026-11-05T08:00:00Z";
    event.quality_status = "VALID";
    event.schema_version = "1";
    return event;
}

void expect_category(std::string_view name,
                     std::string_view event_type,
                     EventCategory expected) {
    const Event event = make_event(event_type);
    const EventCategory actual = classify_event(event);
    const bool passed = actual == expected;
    std::cout << (passed ? "PASS" : "FAIL") << " - " << name << '\n';
    if (!passed) {
        std::cout << "  event_type=" << event_type
                  << ", actual=" << static_cast<int>(actual)
                  << ", expected=" << static_cast<int>(expected) << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    // Все десять типов из согласованного каталога должны получить свою категорию.
    expect_category("STARTUP_REQUESTED is COMMAND", "STARTUP_REQUESTED",
                    EventCategory::Command);
    expect_category("COMMAND_REQUESTED is COMMAND", "COMMAND_REQUESTED",
                    EventCategory::Command);
    expect_category("INITIALIZATION_COMPLETED is SUBSYSTEM", "INITIALIZATION_COMPLETED",
                    EventCategory::Subsystem);
    expect_category("SELF_TEST_COMPLETED is SUBSYSTEM", "SELF_TEST_COMPLETED",
                    EventCategory::Subsystem);
    expect_category("SUBSYSTEM_STATUS_UPDATED is SUBSYSTEM", "SUBSYSTEM_STATUS_UPDATED",
                    EventCategory::Subsystem);
    expect_category("OPERATION_STATUS_UPDATED is OPERATION", "OPERATION_STATUS_UPDATED",
                    EventCategory::Operation);
    expect_category("SAFETY_PERMISSION_UPDATED is SAFETY", "SAFETY_PERMISSION_UPDATED",
                    EventCategory::Safety);
    expect_category("OPERATION_TIMEOUT is TIMER", "OPERATION_TIMEOUT",
                    EventCategory::Timer);
    expect_category("RECOVERY_REQUESTED is RECOVERY", "RECOVERY_REQUESTED",
                    EventCategory::Recovery);
    expect_category("RECOVERY_RECONCILIATION_COMPLETED is RECOVERY",
                    "RECOVERY_RECONCILIATION_COMPLETED", EventCategory::Recovery);

    // Неизвестные, пустые и похожие, но не совпадающие имена не угадываются.
    expect_category("unknown event type", "SENSOR_FAULT", EventCategory::Unknown);
    expect_category("empty event type", "", EventCategory::Unknown);
    expect_category("case mismatch remains unknown", "startup_requested",
                    EventCategory::Unknown);
    expect_category("near-match remains unknown", "SUBSYSTEM_STATUS_UPDATE",
                    EventCategory::Unknown);

    // Повторный вызов с тем же событием даёт тот же результат.
    const Event repeated_event = make_event("OPERATION_TIMEOUT");
    const EventCategory first = classify_event(repeated_event);
    const EventCategory second = classify_event(repeated_event);
    const bool deterministic = first == second && second == EventCategory::Timer;
    std::cout << (deterministic ? "PASS" : "FAIL")
              << " - repeated call is deterministic\n";
    if (!deterministic) {
        ++failures;
    }

    std::cout << "\nTests failed: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
