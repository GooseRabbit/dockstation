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


EventCategory classify_event(const Event& event) noexcept {
    // Сопоставляем только точные зарегистрированные имена: похожие строки не угадываем.
    if (event.event_type == "STARTUP_REQUESTED" ||
        event.event_type == "COMMAND_REQUESTED") {
        return EventCategory::Command;
    }

    // Эти события сообщают об инициализации или состоянии отдельной подсистемы.
    // Неисправность подсистемы также передаётся через SUBSYSTEM_STATUS_UPDATED.
    if (event.event_type == "INITIALIZATION_COMPLETED" ||
        event.event_type == "SELF_TEST_COMPLETED" ||
        event.event_type == "SUBSYSTEM_STATUS_UPDATED") {
        return EventCategory::Subsystem;
    }

    // Изменение статуса операции не является самостоятельным глобальным переходом.
    if (event.event_type == "OPERATION_STATUS_UPDATED") {
        return EventCategory::Operation;
    }

    // Результат SafetyManager классифицируется отдельно от команды и операции.
    if (event.event_type == "SAFETY_PERMISSION_UPDATED") {
        return EventCategory::Safety;
    }

    // Тайм-аут — сигнал времени; он сам по себе не доказывает исход физического действия.
    if (event.event_type == "OPERATION_TIMEOUT") {
        return EventCategory::Timer;
    }

    // События восстановления относятся к сверке состояния после перезапуска/сбоя.
    if (event.event_type == "RECOVERY_REQUESTED" ||
        event.event_type == "RECOVERY_RECONCILIATION_COMPLETED") {
        return EventCategory::Recovery;
    }

    // Неизвестный или пустой тип нельзя автоматически считать событием подсистемы.
    return EventCategory::Unknown;
}

}  // namespace dockstation::core
