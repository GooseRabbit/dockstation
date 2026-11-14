// Run from the repository root:
// g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -Isrc verification/regression/test_source_cursor_reader.cpp src/core/event_processing.cpp -o source_cursor_reader_contract
// ./source_cursor_reader_contract

#include "core/event_processing.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using dockstation::core::SourceCursor;
using dockstation::core::SourceCursorReadResult;
using dockstation::core::SourceCursorReadStatus;
using dockstation::core::SourceCursorReader;
using dockstation::core::is_valid_source_cursor_read_result;

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

// Test-only fixture: owns its strings so returned string_view values have a
// stable backing store. It is not a production persistence adapter.
class StoredSourceCursorReader final : public SourceCursorReader {
public:
    StoredSourceCursorReader(SourceCursorReadStatus status,
                             std::string source,
                             std::optional<std::string> source_instance,
                             std::optional<std::uint64_t> last_sequence)
        : status_(status),
          source_(std::move(source)),
          source_instance_(std::move(source_instance)),
          last_sequence_(last_sequence) {}

    [[nodiscard]] SourceCursorReadResult read_source_cursor(
        std::string_view requested_source) noexcept override {
        ++read_count_;

        if (status_ != SourceCursorReadStatus::Found) {
            return {status_, {}};
        }
        if (requested_source.empty()) {
            return {SourceCursorReadStatus::Error, {}};
        }
        if (requested_source != std::string_view{source_}) {
            return {SourceCursorReadStatus::NotFound, {}};
        }

        std::optional<std::string_view> source_instance_view{};
        if (source_instance_.has_value()) {
            source_instance_view = std::string_view{*source_instance_};
        }

        return {SourceCursorReadStatus::Found,
                SourceCursor{std::string_view{source_},
                             source_instance_view,
                             last_sequence_}};
    }

    [[nodiscard]] int read_count() const noexcept { return read_count_; }

private:
    SourceCursorReadStatus status_;
    std::string source_;
    std::optional<std::string> source_instance_;
    std::optional<std::uint64_t> last_sequence_;
    int read_count_{0};
};

bool is_empty_cursor(const SourceCursor& cursor) noexcept {
    return cursor.source.empty() &&
           !cursor.source_instance.has_value() &&
           !cursor.last_sequence.has_value();
}

void check_default_result_is_fail_closed() {
    const SourceCursorReadResult result{};
    expect(result.status == SourceCursorReadStatus::Error,
           "default result is Error, not NotFound");
    expect(is_empty_cursor(result.cursor),
           "default Error result carries an empty cursor");
}

void check_found_returns_one_exact_snapshot() {
    StoredSourceCursorReader reader(SourceCursorReadStatus::Found,
                                    "esp32/main",
                                    std::string{"boot-17"},
                                    41U);

    const SourceCursorReadResult result = reader.read_source_cursor("esp32/main");
    expect(result.status == SourceCursorReadStatus::Found,
           "known source returns Found");
    expect(result.cursor.source == "esp32/main",
           "Found preserves the exact source key");
    expect(result.cursor.source_instance.has_value() &&
               *result.cursor.source_instance == "boot-17",
           "Found preserves the source instance");
    expect(result.cursor.last_sequence.has_value() &&
               *result.cursor.last_sequence == 41U,
           "Found preserves the last accepted sequence");
    expect(is_valid_source_cursor_read_result("esp32/main", result),
           "the validator accepts a structurally consistent Found result");

    const std::string_view source_view = result.cursor.source;
    const std::string_view instance_view = *result.cursor.source_instance;
    const SourceCursorReadResult repeated = reader.read_source_cursor("esp32/main");
    expect(repeated.status == SourceCursorReadStatus::Found && reader.read_count() == 2,
           "repeated reads return the same read-only result");
    expect(source_view == "esp32/main" && instance_view == "boot-17",
           "previous string_view snapshot remains valid after another read");
}

void check_preestablished_cursor_may_have_no_sequence() {
    StoredSourceCursorReader reader(SourceCursorReadStatus::Found,
                                    "esp32/main",
                                    std::string{"boot-18"},
                                    std::nullopt);
    const SourceCursorReadResult result = reader.read_source_cursor("esp32/main");
    expect(result.status == SourceCursorReadStatus::Found,
           "pre-established source session returns Found");
    expect(result.cursor.source_instance.has_value() &&
               *result.cursor.source_instance == "boot-18",
           "pre-established session identity is retained");
    expect(!result.cursor.last_sequence.has_value(),
           "a confirmed session may have no accepted sequence yet");
}

void check_not_found_is_empty_and_distinct_from_error() {
    StoredSourceCursorReader missing(SourceCursorReadStatus::NotFound,
                                     "",
                                     std::nullopt,
                                     std::nullopt);
    const SourceCursorReadResult not_found = missing.read_source_cursor("esp32/new");
    expect(not_found.status == SourceCursorReadStatus::NotFound,
           "missing source returns NotFound");
    expect(is_empty_cursor(not_found.cursor),
           "NotFound carries an empty cursor payload");

    StoredSourceCursorReader failed(SourceCursorReadStatus::Error,
                                    "",
                                    std::nullopt,
                                    std::nullopt);
    const SourceCursorReadResult error = failed.read_source_cursor("esp32/new");
    expect(error.status == SourceCursorReadStatus::Error,
           "failed read remains Error rather than becoming NotFound");
    expect(is_empty_cursor(error.cursor),
           "Error carries no untrusted cursor data");
    expect(not_found.status != error.status,
           "NotFound and Error remain distinguishable");
    expect(is_valid_source_cursor_read_result("esp32/new", not_found) &&
               is_valid_source_cursor_read_result("esp32/new", error),
           "empty NotFound and Error payloads are structurally valid outcomes");
}

void check_result_validator_rejects_contradictions() {
    SourceCursorReadResult wrong_found{};
    wrong_found.status = SourceCursorReadStatus::Found;
    wrong_found.cursor.source = "esp32/other";
    expect(!is_valid_source_cursor_read_result("esp32/main", wrong_found),
           "Found with a source different from the lookup key is rejected");

    SourceCursorReadResult populated_not_found{};
    populated_not_found.status = SourceCursorReadStatus::NotFound;
    populated_not_found.cursor.source = "esp32/main";
    expect(!is_valid_source_cursor_read_result("esp32/main", populated_not_found),
           "NotFound with cursor data is rejected as contradictory");

    SourceCursorReadResult populated_error{};
    populated_error.status = SourceCursorReadStatus::Error;
    populated_error.cursor.last_sequence = 10U;
    expect(!is_valid_source_cursor_read_result("esp32/main", populated_error),
           "Error with cursor data is rejected as contradictory");

    SourceCursorReadResult unknown_status{};
    unknown_status.status = static_cast<SourceCursorReadStatus>(255U);
    expect(!is_valid_source_cursor_read_result("esp32/main", unknown_status),
           "unknown status values are rejected");
    expect(!is_valid_source_cursor_read_result("", unknown_status),
           "an empty requested source is rejected");
}

void check_unknown_and_empty_keys_fail_closed() {
    StoredSourceCursorReader reader(SourceCursorReadStatus::Found,
                                    "esp32/main",
                                    std::string{"boot-17"},
                                    41U);
    const SourceCursorReadResult wrong_source = reader.read_source_cursor("esp32/other");
    expect(wrong_source.status == SourceCursorReadStatus::NotFound &&
               is_empty_cursor(wrong_source.cursor),
           "a different source key does not receive another source cursor");

    const SourceCursorReadResult empty_source = reader.read_source_cursor("");
    expect(empty_source.status == SourceCursorReadStatus::Error &&
               is_empty_cursor(empty_source.cursor),
           "an empty lookup key is rejected as Error");
}

}  // namespace

int main() {
    check_default_result_is_fail_closed();
    check_found_returns_one_exact_snapshot();
    check_preestablished_cursor_may_have_no_sequence();
    check_not_found_is_empty_and_distinct_from_error();
    check_result_validator_rejects_contradictions();
    check_unknown_and_empty_keys_fail_closed();

    if (failures != 0) {
        std::cerr << failures << " of " << checks << " checks failed.\n";
        return 1;
    }

    std::cout << "All " << checks << " SourceCursorReader contract checks passed.\n";
    return 0;
}
