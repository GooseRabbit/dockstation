#include <cstdlib>
#include <iostream>
#include <string_view>

#include "core/event_processor_router.hpp"

using namespace dockstation::core;

namespace {

int checks = 0;
int failures = 0;

void expect(bool condition, std::string_view description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cout << "FAIL: " << description << '\n';
    }
}

Event make_event(std::string_view type) {
    Event event{};
    event.event_id = "event-1";
    event.event_type = type;
    event.source = "source-1";
    event.source_instance = "boot-1";
    event.received_at = "received";
    event.source_sequence = 11;
    event.quality_status = "VALID";
    event.schema_version = "1";
    return event;
}

SourceCursor make_cursor() {
    return {"source-1", std::string_view{"boot-1"}, 10};
}

class FakeProcessor final : public EventProcessorPort {
public:
    EventProcessorStatus result{EventProcessorStatus::Processed};
    int calls{0};
    const Event* seen_event{nullptr};
    EventCategory seen_category{EventCategory::Unknown};
    const SourceCursor* seen_cursor{nullptr};

    EventProcessorStatus process(
        const Event& event,
        EventCategory category,
        const SourceCursor& expected_cursor) noexcept override {
        ++calls;
        seen_event = &event;
        seen_category = category;
        seen_cursor = &expected_cursor;
        return result;
    }
};

}  // namespace

int main() {
    // Роутер передаёт событие единственному точному обработчику.
    {
        FakeProcessor global;
        FakeProcessor operation;
        FakeProcessor timeout;
        const EventProcessorRoute routes[] = {
            {"STARTUP_REQUESTED", EventCategory::Command, &global},
            {"INITIALIZATION_COMPLETED", EventCategory::Subsystem, &global},
            {"SELF_TEST_COMPLETED", EventCategory::Subsystem, &global},
            {"OPERATION_STATUS_UPDATED", EventCategory::Operation, &operation},
            {"OPERATION_TIMEOUT", EventCategory::Timer, &timeout}
        };
        EventProcessorRouter router(routes, sizeof(routes) / sizeof(routes[0]));
        const SourceCursor cursor = make_cursor();
        Event event = make_event("OPERATION_TIMEOUT");

        expect(router.is_configured(), "valid route table is accepted");
        expect(router.process(event, EventCategory::Timer, cursor) == EventProcessorStatus::Processed,
               "timeout pair is dispatched");
        expect(timeout.calls == 1 && global.calls == 0 && operation.calls == 0,
               "only selected profile is called once");
        expect(timeout.seen_event == &event && timeout.seen_category == EventCategory::Timer,
               "event and category are forwarded unchanged");
        expect(timeout.seen_cursor == &cursor,
               "the exact cursor object is forwarded unchanged");

        event = make_event("STARTUP_REQUESTED");
        expect(router.process(event, EventCategory::Command, cursor) == EventProcessorStatus::Processed,
               "startup command pair is dispatched");
        expect(global.calls == 1 && timeout.calls == 1 && operation.calls == 0,
               "startup command reaches only the global transition processor");

        event = make_event("INITIALIZATION_COMPLETED");
        expect(router.process(event, EventCategory::Subsystem, cursor) == EventProcessorStatus::Processed,
               "initialization subsystem pair is dispatched");

        event = make_event("SELF_TEST_COMPLETED");
        expect(router.process(event, EventCategory::Subsystem, cursor) == EventProcessorStatus::Processed,
               "self-test subsystem pair is dispatched");
        expect(global.calls == 3 && timeout.calls == 1,
               "all three startup pairs reach the global transition processor");

        event = make_event("OPERATION_STATUS_UPDATED");
        expect(router.process(event, EventCategory::Operation, cursor) == EventProcessorStatus::Processed,
               "operation update pair is dispatched");
        expect(operation.calls == 1 && global.calls == 3 && timeout.calls == 1,
               "operation update does not invoke other profiles");
    }

    // Нет точного маршрута — ни один профиль не вызывается.
    {
        FakeProcessor processor;
        const EventProcessorRoute routes[] = {
            {"OPERATION_TIMEOUT", EventCategory::Timer, &processor}
        };
        EventProcessorRouter router(routes, 1U);
        Event event = make_event("OPERATION_TIMEOUT");
        const SourceCursor cursor = make_cursor();

        expect(router.process(event, EventCategory::Operation, cursor) == EventProcessorStatus::Unsupported,
               "wrong category is not routed");
        event = make_event("operation_timeout");
        expect(router.process(event, EventCategory::Timer, cursor) == EventProcessorStatus::Unsupported,
               "case-mismatched event type is not routed");
        expect(processor.calls == 0, "unmatched events have no downstream effects");
    }

    // Статус выбранного профиля возвращается без искажения, кроме Unsupported.
    {
        const EventProcessorStatus forwarded[] = {
            EventProcessorStatus::Processed,
            EventProcessorStatus::NoStateChange,
            EventProcessorStatus::Rejected,
            EventProcessorStatus::Duplicate,
            EventProcessorStatus::EventConflict,
            EventProcessorStatus::ConcurrentUpdate,
            EventProcessorStatus::CommittedNotificationNotConfirmed,
            EventProcessorStatus::Error
        };
        for (const EventProcessorStatus status : forwarded) {
            FakeProcessor processor;
            processor.result = status;
            const EventProcessorRoute routes[] = {
                {"OPERATION_TIMEOUT", EventCategory::Timer, &processor}
            };
            EventProcessorRouter router(routes, 1U);
            Event event = make_event("OPERATION_TIMEOUT");
            expect(router.process(event, EventCategory::Timer, make_cursor()) == status,
                   "known profile result is forwarded unchanged");
        }
    }

    // Регистрация точного ключа дважды запрещена: не выбираем обработчик наугад.
    {
        FakeProcessor first;
        FakeProcessor second;
        const EventProcessorRoute routes[] = {
            {"OPERATION_TIMEOUT", EventCategory::Timer, &first},
            {"OPERATION_TIMEOUT", EventCategory::Timer, &second}
        };
        EventProcessorRouter router(routes, 2U);
        Event event = make_event("OPERATION_TIMEOUT");
        expect(!router.is_configured(), "duplicate route makes configuration invalid");
        expect(router.process(event, EventCategory::Timer, make_cursor()) == EventProcessorStatus::Error,
               "duplicate route fails closed");
        expect(first.calls == 0 && second.calls == 0,
               "ambiguous configuration invokes no processor");
    }

    // Пустые/неполные конфигурации не превращаются в пустой успешный роутер.
    {
        EventProcessorRouter empty(nullptr, 0U);
        Event event = make_event("OPERATION_TIMEOUT");
        expect(!empty.is_configured(), "empty route table is invalid");
        expect(empty.process(event, EventCategory::Timer, make_cursor()) == EventProcessorStatus::Error,
               "empty route table fails closed");

        EventProcessorRouter null_table(nullptr, 1U);
        expect(!null_table.is_configured(), "null route table is invalid");
        expect(null_table.process(event, EventCategory::Timer, make_cursor()) == EventProcessorStatus::Error,
               "null route table fails closed");

        FakeProcessor processor;
        const EventProcessorRoute null_processor[] = {
            {"OPERATION_TIMEOUT", EventCategory::Timer, nullptr}
        };
        EventProcessorRouter null_target(null_processor, 1U);
        expect(!null_target.is_configured(), "null profile is invalid");
        expect(null_target.process(event, EventCategory::Timer, make_cursor()) == EventProcessorStatus::Error,
               "null profile fails closed");

        const EventProcessorRoute empty_type[] = {
            {"", EventCategory::Timer, &processor}
        };
        EventProcessorRouter empty_key(empty_type, 1U);
        expect(!empty_key.is_configured(), "empty event type is invalid");

        const EventProcessorRoute unknown_category[] = {
            {"OPERATION_TIMEOUT", EventCategory::Unknown, &processor}
        };
        EventProcessorRouter unknown_key(unknown_category, 1U);
        expect(!unknown_key.is_configured(), "UNKNOWN category is invalid in a route table");

        const EventProcessorRoute invalid_category[] = {
            {"OPERATION_TIMEOUT", static_cast<EventCategory>(255), &processor}
        };
        EventProcessorRouter invalid_key(invalid_category, 1U);
        expect(!invalid_key.is_configured(), "unknown numeric category is invalid in a route table");
    }

    // Зарегистрированный обработчик не должен неожиданно вернуть Unsupported.
    {
        FakeProcessor processor;
        processor.result = EventProcessorStatus::Unsupported;
        const EventProcessorRoute routes[] = {
            {"OPERATION_TIMEOUT", EventCategory::Timer, &processor}
        };
        EventProcessorRouter router(routes, 1U);
        Event event = make_event("OPERATION_TIMEOUT");
        expect(router.process(event, EventCategory::Timer, make_cursor()) == EventProcessorStatus::Error,
               "Unsupported from a selected profile signals a routing contract error");
        expect(processor.calls == 1, "selected profile is called once before contract error is reported");
    }

    // Неизвестный код enum от профиля тоже закрывает обработку.
    {
        FakeProcessor processor;
        processor.result = static_cast<EventProcessorStatus>(255);
        const EventProcessorRoute routes[] = {
            {"OPERATION_TIMEOUT", EventCategory::Timer, &processor}
        };
        EventProcessorRouter router(routes, 1U);
        Event event = make_event("OPERATION_TIMEOUT");
        expect(router.process(event, EventCategory::Timer, make_cursor()) == EventProcessorStatus::Error,
               "unknown profile result fails closed");
    }

    std::cout << "Checks: " << checks << '\n';
    std::cout << "Failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
