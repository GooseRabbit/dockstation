#include "event_processing.hpp"

namespace dockstation::core {

EventFreshnessResult check_event_freshness(
    const Event& event,           // Проверенное событие: из него берутся source, source_instance и source_sequence.
    const SourceCursor& cursor) noexcept {  // Курсор того же источника/сессии; номер может ещё отсутствовать.
    // Сначала проверяем принадлежность события курсору. Курсор обязан быть привязан
    // к тому же источнику; пустой cursor.source не является подтверждением источника.
    if (cursor.source != event.source) {
        return {EventFreshnessStatus::Unordered,
                EventFreshnessReason::SourceMismatch};
    }
    // Идентификаторы сессии должны присутствовать с обеих сторон либо отсутствовать
    // с обеих сторон. Различие может означать перезапуск или неполный контекст.
    if (cursor.source_instance.has_value() != event.source_instance.has_value()) {
        return {EventFreshnessStatus::Unordered,
                EventFreshnessReason::SourceInstanceMismatch};
    }
    if (cursor.source_instance.has_value() &&
        *cursor.source_instance != *event.source_instance) {
        // Другой instance может означать новую сессию или запоздавшее событие старой.
        // Вызывающий слой должен подтвердить новую сессию и сбросить курсор.
        return {EventFreshnessStatus::Unordered,
                EventFreshnessReason::SourceInstanceMismatch};
    }

    // Без source_sequence порядок событий доказать нельзя. Не используем occurred_at и
    // received_at как замену: часы источников могут быть рассинхронизированы.
    if (!event.source_sequence.has_value()) {
        return {EventFreshnessStatus::Unordered,
                EventFreshnessReason::SequenceMissing};
    }

    // Отсутствие last_sequence после сверки источника и сессии означает первое событие
    // этого источника/сессии либо новую сессию, заранее подтверждённую вызывающим слоем.
    if (!cursor.last_sequence.has_value()) {
        return {EventFreshnessStatus::Current,
                EventFreshnessReason::FirstSequence};
    }

    // Возрастающий номер считается актуальным. Равный номер не позволяет установить
    // порядок, а меньший номер в той же сессии считается устаревшим.
    if (*event.source_sequence > *cursor.last_sequence) {
        return {EventFreshnessStatus::Current,
                EventFreshnessReason::SequenceAdvanced};
    }
    if (*event.source_sequence == *cursor.last_sequence) {
        return {EventFreshnessStatus::Unordered,
                EventFreshnessReason::SequenceRepeated};
    }
    return {EventFreshnessStatus::Stale,
            EventFreshnessReason::SequenceRegressed};
}

}  // namespace dockstation::core
