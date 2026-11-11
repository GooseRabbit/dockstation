#include "event_processing.hpp"

namespace dockstation::core {

namespace {

bool same_optional_text(const std::optional<std::string_view>& left,
                        const std::optional<std::string_view>& right) noexcept {
    // Сначала сравниваем наличие значения, затем само содержимое без нормализации.
    return left.has_value() == right.has_value() &&
           (!left.has_value() || *left == *right);
}

bool same_event_content(const Event& left, const Event& right) noexcept {
    // received_at намеренно исключён: его назначает принимающая сторона при каждом приёме.
    // Все остальные поля конверта и непрозрачный payload сравниваются точно.
    return left.event_id == right.event_id &&
           left.event_type == right.event_type &&
           left.source == right.source &&
           same_optional_text(left.source_instance, right.source_instance) &&
           same_optional_text(left.occurred_at, right.occurred_at) &&
           left.source_sequence == right.source_sequence &&
           same_optional_text(left.operation_id, right.operation_id) &&
           same_optional_text(left.correlation_id, right.correlation_id) &&
           same_optional_text(left.payload, right.payload) &&
           left.quality_status == right.quality_status &&
           left.schema_version == right.schema_version &&
           same_optional_text(left.reason_code, right.reason_code) &&
           same_optional_text(left.reason_text, right.reason_text);
}

// NOT_FOUND означает отсутствие всей записи, а не только пустой event_id.
// Проверяем каждый член Event, чтобы повреждённый ответ адаптера не был принят за NEW.
bool is_empty_event(const Event& event) noexcept {
    return event.event_id.empty() &&
           event.event_type.empty() &&
           event.source.empty() &&
           !event.source_instance.has_value() &&
           !event.occurred_at.has_value() &&
           event.received_at.empty() &&
           !event.source_sequence.has_value() &&
           !event.operation_id.has_value() &&
           !event.correlation_id.has_value() &&
           !event.payload.has_value() &&
           event.quality_status.empty() &&
           event.schema_version.empty() &&
           !event.reason_code.has_value() &&
           !event.reason_text.has_value();
}

}  // namespace

DuplicateEventResult check_duplicate_event(
    const Event& event, EventRepository& repository) noexcept {
    // Ищем запись только по идентификатору; неизвестное состояние хранилища закрывает обработку.
    const EventRepositoryLookupResult lookup = repository.find_by_id(event.event_id);

    switch (lookup.status) {
        case EventRepositoryLookupStatus::NotFound:
            if (!is_empty_event(lookup.event)) {
                return {DuplicateEventStatus::RepositoryError};
            }
            return {DuplicateEventStatus::New};

        case EventRepositoryLookupStatus::Found:
            // Несоответствие ключа и найденной записи означает ошибку контракта репозитория.
            if (lookup.event.event_id != event.event_id) {
                return {DuplicateEventStatus::RepositoryError};
            }

            // Один event_id с тем же содержимым — повтор; с другим содержимым — конфликт.
            return {same_event_content(event, lookup.event)
                        ? DuplicateEventStatus::Duplicate
                        : DuplicateEventStatus::Conflict};

        case EventRepositoryLookupStatus::Error:
            return {DuplicateEventStatus::RepositoryError};
    }

    // Неизвестное значение статуса репозитория нельзя считать NEW.
    return {DuplicateEventStatus::RepositoryError};
}


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
