#pragma once

#include <cstddef>
#include <cstdint>

#include "configuration_types.hpp"

namespace dockstation::configuration {

// Не владеющее памятью представление сохранённой записи в виде байтов.
// Наличие byte buffer не выбирает формат: содержимое интерпретирует декодер.
struct ConfigurationRecordView {
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
};

// Результат чтения записи из SettingsRepository.
enum class SettingsReadStatus : std::uint8_t {
    Found,       // Запись найдена; record содержит её байты.
    NotFound,    // Запись отсутствует.
    Unavailable, // Хранилище временно/постоянно недоступно.
    ReadError    // Ошибка чтения, не равная отсутствию записи.
};

struct SettingsReadResult {
    SettingsReadStatus status{SettingsReadStatus::ReadError};
    ConfigurationRecordView record{};
};

// Логический read-only порт получения текущей сохранённой конфигурации.
// record.data должен оставаться доступен до завершения вызова декодера.
// Реальная файловая система, БД, NVM и способ хранения здесь не задаются.
class SettingsRepository {
public:
    virtual ~SettingsRepository() = default;

    [[nodiscard]] virtual SettingsReadResult read_current() noexcept = 0;
};

// Результат преобразования сохранённых байтов в типизированный кандидат.
enum class ConfigurationDecodeStatus : std::uint8_t {
    Decoded,            // Кандидат сформирован; содержательные проверки ещё впереди.
    InvalidRecord,      // Запись не соответствует структуре поддерживаемого декодера.
    UnsupportedVersion, // Декодер достоверно распознал неподдерживаемую версию схемы.
    Error               // Внутренняя ошибка декодирования.
};

struct ConfigurationDecodeResult {
    ConfigurationDecodeStatus status{ConfigurationDecodeStatus::Error};
    ConfigurationCandidate candidate{};
};

// Логический порт декодирования. Он не закрепляет JSON, YAML, бинарный формат
// или конкретную библиотеку. Представления внутри candidate должны оставаться
// действительными всё время использования кандидата/снимка; декодер не должен
// изменять или освобождать опубликованные данные, пока они ещё используются.
class ConfigurationDecoderPort {
public:
    virtual ~ConfigurationDecoderPort() = default;

    [[nodiscard]] virtual ConfigurationDecodeResult decode(
        const ConfigurationRecordView& record) noexcept = 0;
};

}  // namespace dockstation::configuration
