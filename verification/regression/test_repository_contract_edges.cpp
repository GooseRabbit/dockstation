// Run from the repository root:
// g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -Isrc \
//   src/core/event_processing.cpp src/configuration/configuration_load.cpp \
//   verification/regression/test_repository_contract_edges.cpp -o repository_contract_edges
// ./repository_contract_edges

#include "configuration/configuration_manager.hpp"
#include "configuration/configuration_ports.hpp"
#include "core/event_processing.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;
int checks = 0;

void expect(bool condition, std::string_view description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    } else {
        std::cout << "PASS: " << description << '\n';
    }
}

class FakeEventRepository final : public dockstation::core::EventRepository {
public:
    dockstation::core::EventRepositoryLookupResult result{};

    dockstation::core::EventRepositoryLookupResult find_by_id(
        std::string_view) noexcept override {
        return result;
    }
};

class FakeSettingsRepository final : public dockstation::configuration::SettingsRepository {
public:
    dockstation::configuration::SettingsReadResult result{};
    int calls{0};

    dockstation::configuration::SettingsReadResult read_current() noexcept override {
        ++calls;
        return result;
    }
};

class FakeDecoder final : public dockstation::configuration::ConfigurationDecoderPort {
public:
    int calls{0};

    dockstation::configuration::ConfigurationDecodeResult decode(
        const dockstation::configuration::ConfigurationRecordView&) noexcept override {
        ++calls;
        return {};
    }
};

void check_duplicate_event_not_found_contract() {
    using namespace dockstation::core;

    Event incoming{};
    incoming.event_id = "incoming-event";

    FakeEventRepository repository{};
    repository.result.status = EventRepositoryLookupStatus::NotFound;
    repository.result.event = {};

    expect(check_duplicate_event(incoming, repository).status == DuplicateEventStatus::New,
           "NOT_FOUND with a completely empty Event means NEW");

    repository.result.event.source = "unexpected-source";
    expect(check_duplicate_event(incoming, repository).status ==
               DuplicateEventStatus::RepositoryError,
           "NOT_FOUND with a populated source is a repository error");

    repository.result.event = {};
    repository.result.event.payload = std::string_view{};
    expect(check_duplicate_event(incoming, repository).status ==
               DuplicateEventStatus::RepositoryError,
           "NOT_FOUND with a present-but-empty payload is a repository error");

    repository.result.event = {};
    repository.result.event.source_sequence = 0U;
    expect(check_duplicate_event(incoming, repository).status ==
               DuplicateEventStatus::RepositoryError,
           "NOT_FOUND with a present source sequence is a repository error");
}

void check_configuration_repository_contract() {
    using namespace dockstation::configuration;

    static constexpr std::uint8_t record_byte = 0x42U;
    ConfigurationValidationPolicyView policy{};
    ConfigurationIssue issue_buffer[4]{};

    const auto load = [&](SettingsReadStatus read_status,
                          ConfigurationRecordView record,
                          ConfigurationLoadStatus expected,
                          std::string_view description) {
        FakeSettingsRepository repository{};
        repository.result = {read_status, record};
        FakeDecoder decoder{};
        const ConfigurationLoadResult result =
            load_configuration(repository, decoder, policy, issue_buffer, 4U);

        expect(result.status == expected, description);
        expect(repository.calls == 1 && decoder.calls == 0,
               "repository status stops before decoder invocation");
    };

    load(SettingsReadStatus::Unavailable,
         {nullptr, 0U},
         ConfigurationLoadStatus::Unavailable,
         "UNAVAILABLE with an empty record remains UNAVAILABLE");

    load(SettingsReadStatus::Unavailable,
         {&record_byte, 0U},
         ConfigurationLoadStatus::InternalError,
         "UNAVAILABLE with a non-null zero-length record is contradictory");

    load(SettingsReadStatus::Unavailable,
         {nullptr, 1U},
         ConfigurationLoadStatus::InternalError,
         "UNAVAILABLE with a non-zero record size is contradictory");

    load(SettingsReadStatus::ReadError,
         {nullptr, 0U},
         ConfigurationLoadStatus::ReadError,
         "READ_ERROR with an empty record remains READ_ERROR");

    load(SettingsReadStatus::ReadError,
         {&record_byte, 0U},
         ConfigurationLoadStatus::InternalError,
         "READ_ERROR with a non-null zero-length record is contradictory");

    load(SettingsReadStatus::ReadError,
         {nullptr, 1U},
         ConfigurationLoadStatus::InternalError,
         "READ_ERROR with a non-zero record size is contradictory");
}

}  // namespace

namespace dockstation::configuration {

// These definitions satisfy CFG-F05/F06 linkage for this focused early-exit
// contract test. Every test case exits on the repository status before either
// function may be called; they do not replace the production implementations.
ConfigurationValidationResult validate_configuration(
    const ConfigurationCandidate&,
    const ConfigurationValidationPolicyView&,
    ConfigurationIssue*,
    std::size_t) noexcept {
    return {};
}

ConfigurationSnapshotResult build_validated_configuration_snapshot(
    const ConfigurationCandidate&,
    const ConfigurationValidationResult&) noexcept {
    return {};
}

}  // namespace dockstation::configuration

int main() {
    check_duplicate_event_not_found_contract();
    check_configuration_repository_contract();

    if (failures != 0) {
        std::cerr << failures << " of " << checks << " checks failed.\n";
        return 1;
    }

    std::cout << "All " << checks << " repository contract edge checks passed.\n";
    return 0;
}
