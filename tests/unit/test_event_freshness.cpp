#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

#include "core/event_processing.hpp"

using namespace dockstation::core;

namespace {

int failures = 0;

Event make_event(std::uint64_t sequence,
                 std::string_view source = "sensor-A",
                 std::optional<std::string_view> instance = std::nullopt) {
    Event event{};
    event.event_id = "event-test";
    event.event_type = "MEASUREMENT_RECEIVED";
    event.source = source;
    event.source_instance = instance;
    event.source_sequence = sequence;
    event.received_at = "2026-11-05T08:00:00Z";
    event.payload = "{}";
    event.quality_status = "VALID";
    event.schema_version = "1";
    return event;
}

void expect(std::string_view name,
            const EventFreshnessResult& result,
            EventFreshnessStatus expected_status,
            EventFreshnessReason expected_reason) {
    const bool passed = result.status == expected_status &&
                        result.reason == expected_reason;
    std::cout << (passed ? "PASS" : "FAIL") << " - " << name << '\n';
    if (!passed) {
        std::cout << "  actual status=" << static_cast<int>(result.status)
                  << ", reason=" << static_cast<int>(result.reason) << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    // 1. Первое событие привязанного к источнику курсора устанавливает начальную позицию.
    const SourceCursor initial_cursor{"sensor-A", std::nullopt, std::nullopt};
    expect("first sequence for the matching source",
           check_event_freshness(make_event(1), initial_cursor),
           EventFreshnessStatus::Current, EventFreshnessReason::FirstSequence);

    // 2. Курсор без привязки к источнику не может подтвердить порядок события.
    expect("cursor without source identity is unordered",
           check_event_freshness(make_event(1), SourceCursor{}),
           EventFreshnessStatus::Unordered, EventFreshnessReason::SourceMismatch);

    // 3. Больший номер того же источника и сессии считается актуальным.
    const SourceCursor cursor_10{"sensor-A", std::nullopt, 10};
    expect("sequence advanced",
           check_event_freshness(make_event(11), cursor_10),
           EventFreshnessStatus::Current, EventFreshnessReason::SequenceAdvanced);

    // 4. Повтор номера нельзя считать новым упорядоченным событием.
    expect("sequence repeated",
           check_event_freshness(make_event(10), cursor_10),
           EventFreshnessStatus::Unordered, EventFreshnessReason::SequenceRepeated);

    // 5. Меньший номер в той же сессии считается устаревшим.
    expect("sequence regressed",
           check_event_freshness(make_event(9), cursor_10),
           EventFreshnessStatus::Stale, EventFreshnessReason::SequenceRegressed);

    // 6. Без номера последовательности порядок неизвестен; календарное время не заменяет его.
    auto missing_sequence = make_event(0);
    missing_sequence.source_sequence = std::nullopt;
    missing_sequence.occurred_at = "2999-12-31T23:59:59Z";
    expect("missing sequence is unordered even with a future event timestamp",
           check_event_freshness(missing_sequence, cursor_10),
           EventFreshnessStatus::Unordered, EventFreshnessReason::SequenceMissing);

    // 7. Курсор другого источника нельзя использовать для этого события.
    const SourceCursor other_source{"sensor-B", std::nullopt, 10};
    expect("source mismatch",
           check_event_freshness(make_event(11), other_source),
           EventFreshnessStatus::Unordered, EventFreshnessReason::SourceMismatch);

    // 8. Неожиданный ID сессии может быть новым или запоздавшим старым; не угадываем.
    const SourceCursor old_session{"sensor-A", "boot-1", 10};
    expect("unexpected source instance",
           check_event_freshness(make_event(1, "sensor-A", "boot-2"), old_session),
           EventFreshnessStatus::Unordered,
           EventFreshnessReason::SourceInstanceMismatch);

    // 9. Если ID сессии есть только с одной стороны, сравнение ненадёжно.
    expect("incomplete source instance context",
           check_event_freshness(make_event(11, "sensor-A", "boot-1"), cursor_10),
           EventFreshnessStatus::Unordered,
           EventFreshnessReason::SourceInstanceMismatch);

    // 10. Если обе стороны не используют ID сессии, сравниваем номера.
    expect("sequence advanced without source instance",
           check_event_freshness(make_event(12), cursor_10),
           EventFreshnessStatus::Current, EventFreshnessReason::SequenceAdvanced);

    // 11. Первое событие новой сессии принимается после того, как вызывающий слой
    // подтвердил её, сбросил старый номер и сохранил новый ID; номер может быть меньше.
    const SourceCursor confirmed_new_session{"sensor-A", "boot-2", std::nullopt};
    expect("first sequence in confirmed new session",
           check_event_freshness(make_event(1, "sensor-A", "boot-2"),
                                 confirmed_new_session),
           EventFreshnessStatus::Current, EventFreshnessReason::FirstSequence);

    // 12. Если курсор ждёт ID новой сессии, событие без этого ID не принимается.
    const SourceCursor expected_session{"sensor-A", "boot-2", std::nullopt};
    expect("missing source instance for confirmed session",
           check_event_freshness(make_event(1), expected_session),
           EventFreshnessStatus::Unordered,
           EventFreshnessReason::SourceInstanceMismatch);

    // 13. Повторный вызов с теми же данными возвращает тот же результат (чистая функция).
    const auto first_result = check_event_freshness(make_event(11), cursor_10);
    const auto repeated_result = check_event_freshness(make_event(11), cursor_10);
    expect("repeat call is deterministic", repeated_result, first_result.status,
           first_result.reason);

    // 14. Ноль — допустимый первый номер, а не признак отсутствующего значения.
    expect("zero is a valid sequence value",
           check_event_freshness(make_event(0), initial_cursor),
           EventFreshnessStatus::Current, EventFreshnessReason::FirstSequence);

    std::cout << "\nTests failed: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
