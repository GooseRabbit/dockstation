#pragma once

#include <cstddef>
#include <string_view>

#include "event_handling.hpp"

namespace dockstation::core {

// Одна точная пара event_type/category и её профильный обработчик.
// Строка и объект обработчика не владеются маршрутизатором.
struct EventProcessorRoute {
    std::string_view event_type{};
    EventCategory category{EventCategory::Unknown};
    EventProcessorPort* processor{nullptr};
};

// Композиция профильных обработчиков для общего входного контура F13.
// Массив маршрутов и сами обработчики должны жить дольше маршрутизатора
// и не изменяться во время обработки событий.
class EventProcessorRouter final : public EventProcessorPort {
public:
    // routes — неизменяемый массив точных маршрутов; route_count — число элементов.
    // Пустая, неполная или неоднозначная конфигурация не допускается.
    EventProcessorRouter(
        const EventProcessorRoute* routes,
        std::size_t route_count) noexcept;

    // Передаёт событие ровно одному обработчику по точной паре type/category.
    // expected_cursor передаётся выбранному обработчику без изменения.
    [[nodiscard]] EventProcessorStatus process(
        const Event& event,
        EventCategory category,
        const SourceCursor& expected_cursor) noexcept override;

    // Позволяет проверить конфигурацию до запуска приёма событий.
    [[nodiscard]] bool is_configured() const noexcept;

private:
    const EventProcessorRoute* routes_;
    std::size_t route_count_;
    bool configured_;
};

}  // namespace dockstation::core
