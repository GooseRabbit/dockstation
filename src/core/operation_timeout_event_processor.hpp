#pragma once

#include <cstdint>

#include "event_handling.hpp"
#include "operations.hpp"

namespace dockstation::core {

// Результат преобразования непрозрачного payload в контекст тайм-аута.
enum class OperationTimeoutDecodeStatus : std::uint8_t {
    Decoded,              // Тип и достоверность определены адаптером.
    UnsupportedPayload,  // Формат/вариант payload не поддерживается.
    Error                 // Адаптер не смог достоверно интерпретировать данные.
};

// Типизированные сведения, необходимые F11 для безопасного решения.
// UNKNOWN по умолчанию: отсутствие сведений не считается доказательством.
struct OperationTimeoutDecodeResult {
    OperationTimeoutDecodeStatus status{OperationTimeoutDecodeStatus::Error};
    OperationTimeoutType timeout_type{OperationTimeoutType::Unknown};
    DispatchCertainty dispatch_certainty{DispatchCertainty::Unknown};
};

// Логический порт доверенного адаптера формата события Timer.
// Конкретный формат payload здесь не выбирается и не разбирается.
class OperationTimeoutDecoderPort {
public:
    virtual ~OperationTimeoutDecoderPort() = default;

    // event — исходный конверт OPERATION_TIMEOUT с непрозрачным payload.
    // Возвращает тип тайм-аута и подтверждённую оценку возможности отправки.
    [[nodiscard]] virtual OperationTimeoutDecodeResult decode_timeout(
        const Event& event) noexcept = 0;
};

// Профильный обработчик F13 → F11 для событий OPERATION_TIMEOUT / Timer.
// Сам не измеряет время, не меняет глобальное состояние и не управляет оборудованием.
class OperationTimeoutEventProcessor final : public EventProcessorPort {
public:
    // decoder — доверенный адаптер payload; repository — общий атомарный порт F09/F11.
    OperationTimeoutEventProcessor(
        OperationTimeoutDecoderPort& decoder,
        OperationUpdateRepository& repository) noexcept;

    // Принимает только точную пару Timer/OPERATION_TIMEOUT.
    // expected_cursor передаётся в F11 без изменения для повторной проверки/фиксации.
    [[nodiscard]] EventProcessorStatus process(
        const Event& event,
        EventCategory category,
        const SourceCursor& expected_cursor) noexcept override;

private:
    OperationTimeoutDecoderPort& decoder_;  // Не владеет адаптером; срок жизни задаёт вызывающий код.
    OperationUpdateRepository& repository_; // Не владеет репозиторием; срок жизни задаёт вызывающий код.
};

}  // namespace dockstation::core
