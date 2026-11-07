#pragma once

#include "event_handling.hpp"
#include "event_processor_router.hpp"

namespace dockstation::core {

// Единая точка входа событий станции поверх F13 и точной таблицы профилей.
// Объекты профилей и EventRepository не владеются: их срок жизни задаёт вызывающий код.
class StationEventPipeline final {
public:
    // event_repository — общий read-only поиск event_id для F03.
    // global_processor — профиль стартовых переходов (три пары событий).
    // operation_processor — профиль OPERATION_STATUS_UPDATED / Operation.
    // timeout_processor — профиль OPERATION_TIMEOUT / Timer.
    StationEventPipeline(
        EventRepository& event_repository,
        EventProcessorPort& global_processor,
        EventProcessorPort& operation_processor,
        EventProcessorPort& timeout_processor) noexcept;

    // Внутренний маршрутизатор ссылается на таблицу routes_; копирование/перемещение
    // объекта нарушило бы эту связь, поэтому оно запрещено.
    StationEventPipeline(const StationEventPipeline&) = delete;
    StationEventPipeline& operator=(const StationEventPipeline&) = delete;
    StationEventPipeline(StationEventPipeline&&) = delete;
    StationEventPipeline& operator=(StationEventPipeline&&) = delete;

    // Проверяет конфигурацию маршрутов. Вызывается до запуска приёма событий.
    [[nodiscard]] bool is_configured() const noexcept;

    // Выполняет полный F13: F01 -> F02 -> F03 -> F04 -> выбранный профиль.
    // Курсор не изменяется здесь: его продвигает только транзакция выбранного профиля.
    [[nodiscard]] EventHandlingResult handle(
        const Event& event,
        const EventValidationPolicy& validation_policy,
        const SourceCursor& expected_cursor) noexcept;

private:
    // Храним таблицу как часть объекта, чтобы адрес массива был стабилен.
    const EventProcessorRoute routes_[5];
    EventProcessorRouter router_;
    EventRepository& event_repository_;
};

}  // namespace dockstation::core
