#include <cstdlib>
#include <iostream>
#include <type_traits>

#include "configuration/configuration_ports.hpp"

using namespace dockstation::configuration;

namespace {

int failures = 0;
int checks = 0;

void expect(bool condition, const char* name) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << name << '\n';
    } else {
        std::cerr << "FAIL - " << name << '\n';
        ++failures;
    }
}

class FakeSettingsRepository final : public SettingsRepository {
public:
    SettingsReadResult result{};
    int reads{0};

    [[nodiscard]] SettingsReadResult read_current() noexcept override {
        ++reads;
        return result;
    }
};

class FakeConfigurationDecoder final : public ConfigurationDecoderPort {
public:
    ConfigurationDecodeResult result{};
    int decodes{0};
    const ConfigurationRecordView* last_record{nullptr};

    [[nodiscard]] ConfigurationDecodeResult decode(
        const ConfigurationRecordView& record) noexcept override {
        ++decodes;
        last_record = &record;
        return result;
    }
};

}  // namespace

int main() {
    static_assert(std::is_abstract_v<SettingsRepository>,
                  "SettingsRepository must remain an interface");
    static_assert(std::is_abstract_v<ConfigurationDecoderPort>,
                  "ConfigurationDecoderPort must remain an interface");
    static_assert(std::has_virtual_destructor_v<SettingsRepository>,
                  "SettingsRepository needs a virtual destructor");
    static_assert(std::has_virtual_destructor_v<ConfigurationDecoderPort>,
                  "ConfigurationDecoderPort needs a virtual destructor");
    static_assert(std::is_trivially_copyable_v<ConfigurationValueView>,
                  "ConfigurationValueView is a non-owning value view");
    static_assert(!std::is_default_constructible_v<ValidatedConfigurationSnapshot>,
                  "only the validation factory may create a snapshot");
    static_assert(!std::is_constructible_v<ValidatedConfigurationSnapshot,
                                           std::string_view,
                                           std::string_view,
                                           ConfigurationDocumentView>,
                  "snapshot construction must not be public");

    const ConfigurationLoadResult default_load{};
    expect(default_load.status == ConfigurationLoadStatus::InternalError,
           "default load result fails closed");
    expect(!default_load.snapshot.has_value(),
           "default load result has no snapshot");
    expect(default_load.issues.size == 0 && default_load.issues.items == nullptr,
           "default issue list is empty");

    const ConfigurationDecodeResult default_decode{};
    expect(default_decode.status == ConfigurationDecodeStatus::Error,
           "default decode result is an error");
    const SettingsReadResult default_read{};
    expect(default_read.status == SettingsReadStatus::ReadError,
           "default settings read result is an error");
    expect(default_read.record.data == nullptr && default_read.record.size == 0,
           "default record view is empty");

    // Проверяем возможность описать все поддерживаемые примитивные типы.
    ConfigurationValueView null_value{};
    null_value.type = ConfigurationValueType::Null;

    ConfigurationValueView bool_value{};
    bool_value.type = ConfigurationValueType::Boolean;
    bool_value.value.boolean_value = true;

    ConfigurationValueView signed_value{};
    signed_value.type = ConfigurationValueType::SignedInteger;
    signed_value.value.signed_integer_value = -42;

    ConfigurationValueView unsigned_value{};
    unsigned_value.type = ConfigurationValueType::UnsignedInteger;
    unsigned_value.value.unsigned_integer_value = 42U;

    ConfigurationValueView number_value{};
    number_value.type = ConfigurationValueType::Number;
    number_value.value.number_value = 12.5;

    ConfigurationValueView string_value{};
    string_value.type = ConfigurationValueType::String;
    string_value.value.string_value = "station-profile";

    expect(null_value.type == ConfigurationValueType::Null,
           "null configuration value can be represented");
    expect(bool_value.value.boolean_value,
           "boolean configuration value can be represented");
    expect(signed_value.value.signed_integer_value == -42,
           "signed integer configuration value can be represented");
    expect(unsigned_value.value.unsigned_integer_value == 42U,
           "unsigned integer configuration value can be represented");
    expect(number_value.value.number_value == 12.5,
           "numeric configuration value can be represented");
    expect(string_value.value.string_value == "station-profile",
           "string configuration value can be represented");
    expect(ConfigurationValueView{}.type == ConfigurationValueType::Unknown,
           "unspecified value type is Unknown");

    const std::uint8_t opaque_bytes[] = {0xAAU, 0x55U};
    ConfigurationValueView opaque_value{};
    opaque_value.type = ConfigurationValueType::Opaque;
    opaque_value.value.opaque_value = {opaque_bytes, sizeof(opaque_bytes)};
    expect(opaque_value.value.opaque_value.size == 2 &&
               opaque_value.value.opaque_value.data[1] == 0x55U,
           "opaque values can be preserved without interpreting their format");

    const ConfigurationValueView array_items[] = {bool_value, signed_value};
    ConfigurationValueView array_value{};
    array_value.type = ConfigurationValueType::Array;
    array_value.value.array_value = {array_items, 2};
    expect(array_value.value.array_value.size == 2 &&
               array_value.value.array_value.values[1].value.signed_integer_value == -42,
           "array view points to typed child values");

    const ConfigurationObjectEntryView object_entries[] = {
        {"enabled", bool_value},
        {"attempts", unsigned_value}
    };
    ConfigurationValueView object_value{};
    object_value.type = ConfigurationValueType::Object;
    object_value.value.object_value = {object_entries, 2};
    expect(object_value.value.object_value.size == 2 &&
               object_value.value.object_value.entries[0].name == "enabled" &&
               object_value.value.object_value.entries[1].value.value.unsigned_integer_value == 42U,
           "object view preserves named typed fields");

    ConfigurationCandidate candidate{};
    candidate.schema_version = "1";
    candidate.configuration_revision = "rev-7";
    candidate.document.root = object_value;
    expect(candidate.schema_version == "1" &&
               candidate.configuration_revision == "rev-7",
           "candidate carries schema version and revision");
    expect(candidate.document.root.type == ConfigurationValueType::Object,
           "candidate carries a neutral root document view");

    const ConfigurationIssue issue{
        "REQUIRED_FIELD_MISSING", std::string_view{"power.monitoring_channel"},
        ConfigurationIssueSeverity::Error, "Required field is absent"};
    const ConfigurationIssue warning{
        "OPTIONAL_FIELD_OMITTED", std::nullopt,
        ConfigurationIssueSeverity::Warning, "Optional field is not configured"};
    const ConfigurationIssue issue_items[] = {issue, warning};
    const ConfigurationIssueListView issue_list{issue_items, 2};
    expect(issue.field_path.has_value() &&
               *issue.field_path == "power.monitoring_channel",
           "configuration issue can identify a field path");
    expect(warning.severity == ConfigurationIssueSeverity::Warning &&
               !warning.field_path.has_value(),
           "configuration warning may omit a field path");
    expect(issue_list.size == 2 &&
               issue_list.items[0].severity == ConfigurationIssueSeverity::Error &&
               issue_list.items[1].severity == ConfigurationIssueSeverity::Warning,
           "issue list preserves errors and warnings");

    // Создать ValidatedConfigurationSnapshot напрямую нельзя: будущая CFG-F06
    // будет единственной функцией, имеющей доступ к закрытому конструктору.
    expect(!std::is_constructible_v<ValidatedConfigurationSnapshot,
                                    std::string_view,
                                    std::string_view,
                                    ConfigurationDocumentView>,
           "validated snapshot cannot be forged by consumers");

    ConfigurationLoadResult invalid_result{};
    invalid_result.status = ConfigurationLoadStatus::Invalid;
    invalid_result.issues = issue_list;
    expect(invalid_result.status == ConfigurationLoadStatus::Invalid &&
               !invalid_result.snapshot.has_value() &&
               invalid_result.issues.size == 2,
           "invalid result can carry issues without publishing a snapshot");

    // Проверяем контракты портов через тестовые реализации без реального хранилища.
    const std::uint8_t record_bytes[] = {0x01U, 0x02U, 0x03U};
    FakeSettingsRepository repository{};
    repository.result = {SettingsReadStatus::Found, {record_bytes, sizeof(record_bytes)}};
    const SettingsReadResult read_result = repository.read_current();
    expect(repository.reads == 1, "settings repository port dispatches one read");
    expect(read_result.status == SettingsReadStatus::Found &&
               read_result.record.size == sizeof(record_bytes) &&
               read_result.record.data[2] == 0x03U,
           "settings repository returns a non-owning byte record");

    FakeConfigurationDecoder decoder{};
    decoder.result = {ConfigurationDecodeStatus::Decoded, candidate};
    const ConfigurationDecodeResult decode_result = decoder.decode(read_result.record);
    expect(decoder.decodes == 1, "configuration decoder port dispatches one decode");
    expect(decoder.last_record != nullptr &&
               decoder.last_record->data == record_bytes,
           "decoder receives the repository record without format assumptions");
    expect(decode_result.status == ConfigurationDecodeStatus::Decoded &&
               decode_result.candidate.configuration_revision == "rev-7",
           "decoder returns a typed configuration candidate");

    // Все статусы остаются отдельными: отсутствие не смешивается с отказом чтения.
    constexpr ConfigurationLoadStatus load_statuses[] = {
        ConfigurationLoadStatus::Valid,
        ConfigurationLoadStatus::NotFound,
        ConfigurationLoadStatus::Unavailable,
        ConfigurationLoadStatus::ReadError,
        ConfigurationLoadStatus::DecodeError,
        ConfigurationLoadStatus::UnsupportedVersion,
        ConfigurationLoadStatus::Invalid,
        ConfigurationLoadStatus::InternalError
    };
    bool unique_statuses = true;
    for (std::size_t i = 0; i < sizeof(load_statuses) / sizeof(load_statuses[0]); ++i) {
        for (std::size_t j = i + 1; j < sizeof(load_statuses) / sizeof(load_statuses[0]); ++j) {
            if (load_statuses[i] == load_statuses[j]) {
                unique_statuses = false;
            }
        }
    }
    expect(unique_statuses, "all configuration load statuses are distinct");

    constexpr SettingsReadStatus read_statuses[] = {
        SettingsReadStatus::Found,
        SettingsReadStatus::NotFound,
        SettingsReadStatus::Unavailable,
        SettingsReadStatus::ReadError
    };
    bool unique_read_statuses = true;
    for (std::size_t i = 0; i < sizeof(read_statuses) / sizeof(read_statuses[0]); ++i) {
        for (std::size_t j = i + 1; j < sizeof(read_statuses) / sizeof(read_statuses[0]); ++j) {
            if (read_statuses[i] == read_statuses[j]) {
                unique_read_statuses = false;
            }
        }
    }
    expect(unique_read_statuses, "all settings read statuses are distinct");

    constexpr ConfigurationDecodeStatus decode_statuses[] = {
        ConfigurationDecodeStatus::Decoded,
        ConfigurationDecodeStatus::InvalidRecord,
        ConfigurationDecodeStatus::UnsupportedVersion,
        ConfigurationDecodeStatus::Error
    };
    bool unique_decode_statuses = true;
    for (std::size_t i = 0; i < sizeof(decode_statuses) / sizeof(decode_statuses[0]); ++i) {
        for (std::size_t j = i + 1; j < sizeof(decode_statuses) / sizeof(decode_statuses[0]); ++j) {
            if (decode_statuses[i] == decode_statuses[j]) {
                unique_decode_statuses = false;
            }
        }
    }
    expect(unique_decode_statuses, "all configuration decode statuses are distinct");

    std::cout << "\n" << (failures == 0 ? "PASS" : "FAIL")
              << " - " << checks << " checks; failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
