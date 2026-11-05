#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

#include "core/event_processing.hpp"

using namespace dockstation::core;

namespace {

int failures = 0;

Event make_event() {
    Event event{};
    event.event_id = "event-100";
    event.event_type = "OPERATION_STATUS_UPDATED";
    event.source = "executor-A";
    event.source_instance = "boot-7";
    event.occurred_at = "2026-11-05T09:00:00Z";
    event.received_at = "2026-11-05T09:00:01Z";
    event.source_sequence = 42;
    event.operation_id = "operation-12";
    event.correlation_id = "correlation-3";
    event.payload = "{\"status\":\"STARTED\"}";
    event.quality_status = "VALID";
    event.schema_version = "1";
    event.reason_code = "MOTOR_STARTED";
    event.reason_text = "Operation started";
    return event;
}

class FakeEventRepository final : public EventRepository {
public:
    EventRepositoryLookupStatus response_status{EventRepositoryLookupStatus::NotFound};
    Event stored_event{};
    bool return_mismatched_key{false};
    int lookup_count{0};

    EventRepositoryLookupResult find_by_id(std::string_view event_id) noexcept override {
        ++lookup_count;
        if (response_status == EventRepositoryLookupStatus::Found) {
            if (!return_mismatched_key && stored_event.event_id != event_id) {
                return {EventRepositoryLookupStatus::NotFound, {}};
            }
            return {EventRepositoryLookupStatus::Found, stored_event};
        }
        return {response_status, {}};
    }
};

void expect_status(std::string_view name,
                   DuplicateEventStatus actual,
                   DuplicateEventStatus expected) {
    const bool passed = actual == expected;
    std::cout << (passed ? "PASS" : "FAIL") << " - " << name << '\n';
    if (!passed) {
        std::cout << "  actual=" << static_cast<int>(actual)
                  << ", expected=" << static_cast<int>(expected) << '\n';
        ++failures;
    }
}

void expect_against_stored(std::string_view name,
                           const Event& incoming,
                           DuplicateEventStatus expected) {
    FakeEventRepository repository;
    repository.response_status = EventRepositoryLookupStatus::Found;
    repository.stored_event = make_event();
    const auto result = check_duplicate_event(incoming, repository);
    expect_status(name, result.status, expected);
    if (repository.lookup_count != 1) {
        std::cout << "  repository lookup count=" << repository.lookup_count << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    // 1. Отсутствующая запись означает NEW.
    {
        FakeEventRepository repository;
        const auto result = check_duplicate_event(make_event(), repository);
        expect_status("event id not found is NEW", result.status, DuplicateEventStatus::New);
        if (repository.lookup_count != 1) ++failures;
    }

    // 2. Совпадающий идентификатор и совпадающее содержимое означают DUPLICATE.
    {
        FakeEventRepository repository;
        repository.response_status = EventRepositoryLookupStatus::Found;
        repository.stored_event = make_event();
        const auto result = check_duplicate_event(make_event(), repository);
        expect_status("same id and same content is DUPLICATE",
                      result.status, DuplicateEventStatus::Duplicate);
    }

    // 3. received_at назначается получателем и не меняет идентичность исходного события.
    {
        Event incoming = make_event();
        incoming.received_at = "2026-11-05T09:05:00Z";
        expect_against_stored("different receiver timestamp is still DUPLICATE",
                              incoming, DuplicateEventStatus::Duplicate);
    }

    // 4–15. Изменение любого содержательного поля при том же event_id — CONFLICT.
    {
        Event e = make_event(); e.event_type = "COMMAND_REQUESTED";
        expect_against_stored("different event_type is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.source = "executor-B";
        expect_against_stored("different source is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.source_instance = "boot-8";
        expect_against_stored("different source_instance is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.occurred_at = "2026-11-05T09:01:00Z";
        expect_against_stored("different occurred_at is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.source_sequence = 43;
        expect_against_stored("different source_sequence is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.operation_id = "operation-13";
        expect_against_stored("different operation_id is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.correlation_id = "correlation-4";
        expect_against_stored("different correlation_id is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.payload = "{\"status\":\"COMPLETED\"}";
        expect_against_stored("different payload is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.payload = std::nullopt;
        expect_against_stored("missing payload is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.quality_status = "UNKNOWN";
        expect_against_stored("different quality_status is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.schema_version = "2";
        expect_against_stored("different schema_version is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.reason_code = "MOTOR_STOPPED";
        expect_against_stored("different reason_code is CONFLICT", e, DuplicateEventStatus::Conflict);
    }
    {
        Event e = make_event(); e.reason_text = "Different reason";
        expect_against_stored("different reason_text is CONFLICT", e, DuplicateEventStatus::Conflict);
    }

    // Ошибка чтения и неизвестный ответ репозитория не трактуются как отсутствие записи.
    {
        FakeEventRepository repository;
        repository.response_status = EventRepositoryLookupStatus::Error;
        const auto result = check_duplicate_event(make_event(), repository);
        expect_status("repository read error fails closed",
                      result.status, DuplicateEventStatus::RepositoryError);
    }
    {
        FakeEventRepository repository;
        repository.response_status = static_cast<EventRepositoryLookupStatus>(255);
        const auto result = check_duplicate_event(make_event(), repository);
        expect_status("unknown repository status fails closed",
                      result.status, DuplicateEventStatus::RepositoryError);
    }

    // Найденная запись с другим event_id — нарушение контракта хранилища.
    {
        FakeEventRepository repository;
        repository.response_status = EventRepositoryLookupStatus::Found;
        repository.stored_event = make_event();
        repository.stored_event.event_id = "different-id";
        repository.return_mismatched_key = true;
        const auto result = check_duplicate_event(make_event(), repository);
        expect_status("repository key mismatch is an error",
                      result.status, DuplicateEventStatus::RepositoryError);
    }

    // Проверка не меняет входное событие и не записывает его в хранилище.
    {
        Event event = make_event();
        const std::string_view original_id = event.event_id;
        const std::string_view original_payload = *event.payload;
        FakeEventRepository repository;
        const auto result = check_duplicate_event(event, repository);
        expect_status("read-only check returns NEW", result.status, DuplicateEventStatus::New);
        if (event.event_id != original_id || *event.payload != original_payload) {
            std::cout << "FAIL - input event remains unchanged\n";
            ++failures;
        } else {
            std::cout << "PASS - input event remains unchanged\n";
        }
    }

    // Повторный вызов с тем же неизменным репозиторием детерминирован.
    {
        Event event = make_event();
        FakeEventRepository repository;
        repository.response_status = EventRepositoryLookupStatus::Found;
        repository.stored_event = make_event();
        const auto first = check_duplicate_event(event, repository);
        const auto second = check_duplicate_event(event, repository);
        expect_status("repeated lookup has the same result",
                      second.status, first.status);
        if (repository.lookup_count != 2) ++failures;
    }

    std::cout << "\nTests failed: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
