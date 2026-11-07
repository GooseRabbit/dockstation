#include "event_processor_router.hpp"

namespace dockstation::core {
namespace {

bool is_known_category(EventCategory category) noexcept {
    switch (category) {
        case EventCategory::Subsystem:
        case EventCategory::Operation:
        case EventCategory::Safety:
        case EventCategory::Command:
        case EventCategory::Timer:
        case EventCategory::Recovery:
            return true;
        case EventCategory::Unknown:
            return false;
    }
    return false;
}

bool is_known_processor_status(EventProcessorStatus status) noexcept {
    switch (status) {
        case EventProcessorStatus::Processed:
        case EventProcessorStatus::NoStateChange:
        case EventProcessorStatus::Rejected:
        case EventProcessorStatus::Unsupported:
        case EventProcessorStatus::Duplicate:
        case EventProcessorStatus::EventConflict:
        case EventProcessorStatus::ConcurrentUpdate:
        case EventProcessorStatus::CommittedNotificationNotConfirmed:
        case EventProcessorStatus::Error:
            return true;
    }
    return false;
}

bool has_valid_routes(const EventProcessorRoute* routes, std::size_t route_count) noexcept {
    if (routes == nullptr || route_count == 0U) {
        return false;
    }

    for (std::size_t i = 0; i < route_count; ++i) {
        const EventProcessorRoute& route = routes[i];
        if (route.event_type.empty() || !is_known_category(route.category) ||
            route.processor == nullptr) {
            return false;
        }

        // Один ключ маршрутизации обязан указывать ровно на один обработчик.
        for (std::size_t j = 0; j < i; ++j) {
            if (routes[j].event_type == route.event_type &&
                routes[j].category == route.category) {
                return false;
            }
        }
    }

    return true;
}

}  // namespace

EventProcessorRouter::EventProcessorRouter(
    const EventProcessorRoute* routes,
    std::size_t route_count) noexcept
    : routes_(routes), route_count_(route_count),
      configured_(has_valid_routes(routes, route_count)) {}

bool EventProcessorRouter::is_configured() const noexcept {
    return configured_;
}

EventProcessorStatus EventProcessorRouter::process(
    const Event& event,
    EventCategory category,
    const SourceCursor& expected_cursor) noexcept {
    // Ошибка конфигурации блокирует маршрутизацию до любого вызова профиля.
    if (!configured_) {
        return EventProcessorStatus::Error;
    }

    const EventProcessorRoute* selected = nullptr;
    for (std::size_t i = 0; i < route_count_; ++i) {
        const EventProcessorRoute& route = routes_[i];
        if (route.event_type == event.event_type && route.category == category) {
            // Защита от неоднозначности даже если внешний массив изменили после ctor.
            if (selected != nullptr) {
                return EventProcessorStatus::Error;
            }
            selected = &route;
        }
    }

    if (selected == nullptr) {
        return EventProcessorStatus::Unsupported;
    }

    const EventProcessorStatus result =
        selected->processor->process(event, category, expected_cursor);

    // UNKNOWN enum из профиля не должен выходить из маршрутизатора как успех.
    if (!is_known_processor_status(result)) {
        return EventProcessorStatus::Error;
    }

    // UNSUPPORTED от зарегистрированного точного маршрута означает, что
    // конфигурация и контракт самого обработчика расходятся.
    if (result == EventProcessorStatus::Unsupported) {
        return EventProcessorStatus::Error;
    }

    return result;
}

}  // namespace dockstation::core
