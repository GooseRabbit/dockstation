#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dockstation::configuration {

// Все типы с суффиксом View, а также std::string_view и списки с указателями,
// не владеют памятью. Буферы и строки должны оставаться действительными всё
// время использования кандидата, результата, проблемы или снимка. Реальные
// адаптеры обязаны явно обеспечить этот срок жизни; формат хранения не задаётся.

// Состояние результата полной загрузки и проверки конфигурации.
enum class ConfigurationLoadStatus : std::uint8_t {
    Valid,               // Конфигурация прошла все обязательные проверки.
    NotFound,            // Сохранённая конфигурация отсутствует.
    Unavailable,         // Хранилище или обязательная зависимость недоступны.
    ReadError,           // Не удалось достоверно прочитать сохранённую запись.
    DecodeError,         // Сохранённая запись имеет некорректную структуру/содержимое.
    UnsupportedVersion,  // Версия схемы не поддерживается.
    Invalid,             // Декодированная конфигурация не прошла обязательные проверки.
    InternalError        // Внутренняя ошибка обработки.
};

// Уровень отдельной проблемы конфигурации.
enum class ConfigurationIssueSeverity : std::uint8_t {
    Error,    // Нарушено обязательное условие; итог VALID невозможен.
    Warning   // Необязательное замечание, не блокирующее VALID.
};

// Тип значения в нейтральном, не зависящем от формата хранения дереве.
enum class ConfigurationValueType : std::uint8_t {
    Unknown,          // Тип не задан или не распознан.
    Null,
    Boolean,
    SignedInteger,
    UnsignedInteger,
    Number,
    String,
    Object,
    Array,
    Opaque            // Непроинтерпретированное значение, сохранённое как байтовый фрагмент.
};

struct ConfigurationValueView;
struct ConfigurationObjectEntryView;

// Непроинтерпретированный фрагмент данных. Содержимое не трактуется как формат.
struct ConfigurationOpaqueView {
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
};

// Нейтральное представление массива. Память принадлежит декодеру.
struct ConfigurationArrayView {
    const ConfigurationValueView* values{nullptr};
    std::size_t size{0};
};

// Нейтральное представление объекта. Память принадлежит декодеру.
struct ConfigurationObjectView {
    const ConfigurationObjectEntryView* entries{nullptr};
    std::size_t size{0};
};

// Хранилище активного значения. Интерпретируется только согласно type.
union ConfigurationValueStorage {
    bool boolean_value;
    std::int64_t signed_integer_value;
    std::uint64_t unsigned_integer_value;
    double number_value;
    std::string_view string_value;
    ConfigurationObjectView object_value;
    ConfigurationArrayView array_value;
    ConfigurationOpaqueView opaque_value;

    constexpr ConfigurationValueStorage() noexcept : unsigned_integer_value(0) {}
};

// Не владеющее памятью типизированное значение конфигурации.
struct ConfigurationValueView {
    ConfigurationValueType type{ConfigurationValueType::Unknown};
    ConfigurationValueStorage value{};
};

// Одно именованное поле объекта конфигурации.
struct ConfigurationObjectEntryView {
    std::string_view name{};
    ConfigurationValueView value{};
};

// Корневой узел декодированной конфигурации.
struct ConfigurationDocumentView {
    ConfigurationValueView root{};
};

// Декодированная конфигурация-кандидат. Она ещё не прошла содержательную проверку.
struct ConfigurationCandidate {
    std::string_view schema_version{};
    std::string_view configuration_revision{};
    ConfigurationDocumentView document{};
};

// Отдельная проблема проверки. field_path отсутствует, если проблема не относится
// к конкретному полю. Все строки являются представлениями чужой памяти.
struct ConfigurationIssue {
    std::string_view reason_code{};
    std::optional<std::string_view> field_path{};
    ConfigurationIssueSeverity severity{ConfigurationIssueSeverity::Error};
    std::string_view description{};
};

// Не владеющий памятью список проблем проверки.
struct ConfigurationIssueListView {
    const ConfigurationIssue* items{nullptr};
    std::size_t size{0};
};

// Результат одной или нескольких обязательных проверок конфигурации.
enum class ConfigurationValidationStatus : std::uint8_t {
    Valid,               // Все выполненные обязательные проверки пройдены.
    Invalid,             // Данные кандидата нарушают обязательное правило.
    UnsupportedVersion,  // Версия схемы не входит в явный набор поддерживаемых.
    InternalError        // Политика проверки или внутренняя обработка некорректна.
};

// Значение по умолчанию fail-closed: отсутствие результата не означает VALID.
struct ConfigurationValidationResult {
    ConfigurationValidationStatus status{ConfigurationValidationStatus::InternalError};
    ConfigurationIssueListView issues{};
};

// Явный набор допустимых версий схемы. Тип не владеет памятью; массив должен
// оставаться доступным во время проверки. Пустой набор не разрешает ни одну
// версию и считается ошибкой конфигурации политики.
struct ConfigurationSchemaVersionSetView {
    const std::string_view* versions{nullptr};
    std::size_t size{0};
};

struct ConfigurationSnapshotResult;

// Проверенный снимок. Его поля и документ должны относиться к одной ревизии.
// Публичные методы только читают поля; создать снимок может только CFG-F06.
class ValidatedConfigurationSnapshot final {
public:
    [[nodiscard]] std::string_view schema_version() const noexcept {
        return schema_version_;
    }

    [[nodiscard]] std::string_view configuration_revision() const noexcept {
        return configuration_revision_;
    }

    [[nodiscard]] const ConfigurationDocumentView& document() const noexcept {
        return document_;
    }

private:
    friend ConfigurationSnapshotResult build_validated_configuration_snapshot(
        const ConfigurationCandidate& candidate,
        const ConfigurationValidationResult& validation) noexcept;

    ValidatedConfigurationSnapshot(
        std::string_view schema_version,
        std::string_view configuration_revision,
        ConfigurationDocumentView document) noexcept
        : schema_version_(schema_version),
          configuration_revision_(configuration_revision),
          document_(document) {}

    std::string_view schema_version_{};
    std::string_view configuration_revision_{};
    ConfigurationDocumentView document_{};
};

// Результат загрузки ConfigurationManager. Контракт будущей реализации:
// snapshot присутствует только при status == Valid; при остальных статусах отсутствует.
// Если revision/version известны, они относятся к той же записи, что и snapshot/result.
struct ConfigurationLoadResult {
    ConfigurationLoadStatus status{ConfigurationLoadStatus::InternalError};
    std::optional<std::string_view> schema_version{};
    std::optional<std::string_view> configuration_revision{};
    ConfigurationIssueListView issues{};
    std::optional<ValidatedConfigurationSnapshot> snapshot{};
};

}  // namespace dockstation::configuration
