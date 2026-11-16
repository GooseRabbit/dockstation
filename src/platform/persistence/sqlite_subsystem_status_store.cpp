#include "platform/persistence/sqlite_subsystem_status_store.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include <sqlite3.h>

#include "core/event_processing.hpp"
#include "core/event_validation.hpp"

namespace dockstation::platform::persistence {
namespace {

class Statement final {
public:
    Statement() = default;
    ~Statement() {
        if (statement_ != nullptr) {
            (void)sqlite3_finalize(statement_);
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] bool prepare(sqlite3* database, const char* sql) noexcept {
        return sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) == SQLITE_OK;
    }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }
    [[nodiscard]] int step() noexcept { return sqlite3_step(statement_); }
    [[nodiscard]] int finish() noexcept {
        if (statement_ == nullptr) return SQLITE_OK;
        sqlite3_stmt* current = statement_;
        statement_ = nullptr;
        return sqlite3_finalize(current);
    }

private:
    sqlite3_stmt* statement_{nullptr};
};

class Transaction final {
public:
    explicit Transaction(sqlite3* database) noexcept : database_(database) {
        status_ = sqlite3_exec(database_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
        active_ = status_ == SQLITE_OK;
    }
    ~Transaction() { rollback(); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    [[nodiscard]] bool started() const noexcept { return active_; }
    [[nodiscard]] int status() const noexcept { return status_; }
    [[nodiscard]] bool commit() noexcept {
        if (!active_) return false;
        status_ = sqlite3_exec(database_, "COMMIT;", nullptr, nullptr, nullptr);
        if (status_ == SQLITE_OK) active_ = false;
        return !active_;
    }
    void rollback() noexcept {
        if (active_) {
            (void)sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
            active_ = false;
        }
    }

private:
    sqlite3* database_{nullptr};
    int status_{SQLITE_OK};
    bool active_{false};
};

[[nodiscard]] bool is_busy(int status) noexcept {
    const int primary = status & 0xff;
    return primary == SQLITE_BUSY || primary == SQLITE_LOCKED;
}

[[nodiscard]] bool has_non_whitespace(std::string_view value) noexcept {
    for (const unsigned char ch : value) {
        if (ch != ' ' && ch != '\t' && ch != '\n' &&
            ch != '\r' && ch != '\f' && ch != '\v') return true;
    }
    return false;
}

[[nodiscard]] bool valid_identifier(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128U &&
           value.find(static_cast<char>(0)) == std::string_view::npos &&
           has_non_whitespace(value);
}

[[nodiscard]] bool valid_registration_state(
    core::SubsystemRegistrationState state) noexcept {
    return state == core::SubsystemRegistrationState::Active ||
           state == core::SubsystemRegistrationState::Disabled ||
           state == core::SubsystemRegistrationState::Revoked;
}

[[nodiscard]] bool valid_severity(std::string_view severity) noexcept {
    return severity == "INFO" || severity == "WARNING" ||
           severity == "ERROR" || severity == "CRITICAL";
}

[[nodiscard]] bool bind_text(
    sqlite3_stmt* statement, int index, std::string_view value) noexcept {
    return value.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
           sqlite3_bind_text(statement, index, value.data(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

[[nodiscard]] bool bind_optional_text(
    sqlite3_stmt* statement, int index,
    const std::optional<std::string_view>& value) noexcept {
    return value.has_value()
        ? bind_text(statement, index, *value)
        : sqlite3_bind_null(statement, index) == SQLITE_OK;
}

[[nodiscard]] bool copy_required_text(
    sqlite3_stmt* statement, int column, std::string& output) {
    const auto* bytes = sqlite3_column_text(statement, column);
    const int length = sqlite3_column_bytes(statement, column);
    if (bytes == nullptr || length <= 0) return false;
    output.assign(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(length));
    return true;
}

[[nodiscard]] bool copy_optional_text(
    sqlite3_stmt* statement, int column, std::optional<std::string>& output) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        output.reset();
        return true;
    }
    const auto* bytes = sqlite3_column_text(statement, column);
    const int length = sqlite3_column_bytes(statement, column);
    if (bytes == nullptr && length != 0) return false;
    output.emplace(bytes == nullptr ? "" : reinterpret_cast<const char*>(bytes),
                   static_cast<std::size_t>(length));
    return true;
}

[[nodiscard]] bool parse_sequence(
    sqlite3_stmt* statement, int column,
    std::optional<std::uint64_t>& output) noexcept {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        output.reset();
        return true;
    }
    const auto* bytes = sqlite3_column_text(statement, column);
    const int length = sqlite3_column_bytes(statement, column);
    if (bytes == nullptr || length <= 0 || length > 20) return false;

    const char* begin = reinterpret_cast<const char*>(bytes);
    const char* end = begin + length;
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return false;

    std::array<char, 21U> canonical{};
    const auto encoded = std::to_chars(
        canonical.data(), canonical.data() + canonical.size(), value, 10);
    if (encoded.ec != std::errc{} ||
        std::string_view{canonical.data(),
            static_cast<std::size_t>(encoded.ptr - canonical.data())} !=
            std::string_view{begin, static_cast<std::size_t>(length)}) return false;
    output = value;
    return true;
}

[[nodiscard]] bool encode_sequence(
    std::uint64_t value, std::array<char, 21U>& buffer,
    std::string_view& output) noexcept {
    const auto encoded = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, 10);
    if (encoded.ec != std::errc{}) return false;
    output = {buffer.data(), static_cast<std::size_t>(encoded.ptr - buffer.data())};
    return true;
}

[[nodiscard]] std::optional<std::string_view> view_optional(
    const std::optional<std::string>& value) noexcept {
    return value.has_value() ? std::optional<std::string_view>{*value} : std::nullopt;
}

[[nodiscard]] SqliteSubsystemRegistrationWriteResult write_result(
    SqliteSubsystemRegistrationWriteStatus status,
    int sqlite_status = SQLITE_OK) noexcept {
    return {status, sqlite_status};
}

[[nodiscard]] core::SubsystemStatusCommitResult commit_result(
    core::SubsystemStatusCommitStatus status) noexcept {
    return {status};
}

[[nodiscard]] bool report_matches_event(
    const core::DecodedSubsystemStatus& report,
    const core::Event& event) noexcept {
    if (!valid_identifier(report.subsystem_id) ||
        !valid_identifier(report.error_code) ||
        !has_non_whitespace(report.description) ||
        !valid_severity(report.severity) ||
        !core::is_known_reported_subsystem_status(report.status) ||
        report.reason_text.has_value() != event.reason_text.has_value()) return false;

    if (report.reason_text.has_value()) {
        if (!has_non_whitespace(*report.reason_text) ||
            !event.reason_code.has_value() ||
            *event.reason_code != report.error_code ||
            *event.reason_text != *report.reason_text) return false;
    } else if (event.reason_code.has_value()) {
        return false;
    }
    return true;
}

[[nodiscard]] bool bind_event(
    sqlite3_stmt* statement, const core::Event& event,
    std::string_view sequence_text) noexcept {
    return bind_text(statement, 1, event.event_id) &&
           bind_text(statement, 2, event.event_type) &&
           bind_text(statement, 3, event.source) &&
           bind_optional_text(statement, 4, event.source_instance) &&
           bind_optional_text(statement, 5, event.occurred_at) &&
           bind_text(statement, 6, event.received_at) &&
           bind_text(statement, 7, sequence_text) &&
           bind_optional_text(statement, 8, event.operation_id) &&
           bind_optional_text(statement, 9, event.correlation_id) &&
           bind_optional_text(statement, 10, event.payload) &&
           bind_text(statement, 11, event.quality_status) &&
           bind_text(statement, 12, event.schema_version) &&
           bind_optional_text(statement, 13, event.reason_code) &&
           bind_optional_text(statement, 14, event.reason_text);
}

}  // namespace

SqliteSubsystemStatusStore::SqliteSubsystemStatusStore(SqliteDatabase& database) noexcept
    : database_(database) {}

SqliteSubsystemRegistrationWriteResult
SqliteSubsystemStatusStore::create_subsystem_registration(
    std::string_view station_id, std::string_view subsystem_id,
    std::string_view source, std::string_view registration_revision) noexcept {
    if (!valid_identifier(station_id) || !valid_identifier(subsystem_id) ||
        !valid_identifier(source) || !valid_identifier(registration_revision)) {
        return write_result(SqliteSubsystemRegistrationWriteStatus::InvalidArgument,
                            SQLITE_MISUSE);
    }
    if (!database_.is_open() || database_.native_handle() == nullptr) {
        return write_result(SqliteSubsystemRegistrationWriteStatus::Unavailable,
                            SQLITE_MISUSE);
    }

    sqlite3* database = database_.native_handle();
    Transaction transaction{database};
    if (!transaction.started()) {
        const int status = transaction.status();
        return write_result(is_busy(status)
            ? SqliteSubsystemRegistrationWriteStatus::Unavailable
            : SqliteSubsystemRegistrationWriteStatus::Error, status);
    }

    Statement source_registration;
    if (!source_registration.prepare(database,
        "SELECT 1 FROM source_registrations "
        "WHERE station_id=?1 AND source=?2 AND state=1;") ||
        !bind_text(source_registration.get(), 1, station_id) ||
        !bind_text(source_registration.get(), 2, source)) {
        transaction.rollback();
        return write_result(SqliteSubsystemRegistrationWriteStatus::Error,
                            sqlite3_extended_errcode(database));
    }
    const int source_status = source_registration.step();
    const int source_finalize = source_registration.finish();
    if (source_status == SQLITE_DONE && source_finalize == SQLITE_OK) {
        transaction.rollback();
        return write_result(SqliteSubsystemRegistrationWriteStatus::SourceNotRegistered);
    }
    if (source_status != SQLITE_ROW || source_finalize != SQLITE_OK) {
        transaction.rollback();
        return write_result(SqliteSubsystemRegistrationWriteStatus::Error,
                            sqlite3_extended_errcode(database));
    }

    Statement insert;
    if (!insert.prepare(database,
        "INSERT INTO subsystem_registrations("
        "station_id, subsystem_id, source, registration_revision, state) "
        "VALUES(?1, ?2, ?3, ?4, 1);") ||
        !bind_text(insert.get(), 1, station_id) ||
        !bind_text(insert.get(), 2, subsystem_id) ||
        !bind_text(insert.get(), 3, source) ||
        !bind_text(insert.get(), 4, registration_revision)) {
        transaction.rollback();
        return write_result(SqliteSubsystemRegistrationWriteStatus::Error,
                            sqlite3_extended_errcode(database));
    }
    const int insert_status = insert.step();
    const int insert_finalize = insert.finish();
    if (insert_status != SQLITE_DONE || insert_finalize != SQLITE_OK) {
        const int status = insert_status != SQLITE_DONE
            ? sqlite3_extended_errcode(database) : insert_finalize;
        transaction.rollback();
        if ((status & 0xff) == SQLITE_CONSTRAINT) {
            return write_result(SqliteSubsystemRegistrationWriteStatus::Conflict, status);
        }
        return write_result(is_busy(status)
            ? SqliteSubsystemRegistrationWriteStatus::Unavailable
            : SqliteSubsystemRegistrationWriteStatus::Error, status);
    }

    if (!transaction.commit()) {
        const int status = transaction.status();
        transaction.rollback();
        return write_result(is_busy(status)
            ? SqliteSubsystemRegistrationWriteStatus::Unavailable
            : SqliteSubsystemRegistrationWriteStatus::Error, status);
    }
    return write_result(SqliteSubsystemRegistrationWriteStatus::Created);
}

SqliteSubsystemRegistrationWriteResult
SqliteSubsystemStatusStore::change_subsystem_state(
    std::string_view station_id, std::string_view subsystem_id,
    std::string_view expected_revision, std::string_view new_revision,
    core::SubsystemRegistrationState next_state) noexcept {
    if (!valid_identifier(station_id) || !valid_identifier(subsystem_id) ||
        !valid_identifier(expected_revision) || !valid_identifier(new_revision) ||
        expected_revision == new_revision || !valid_registration_state(next_state)) {
        return write_result(SqliteSubsystemRegistrationWriteStatus::InvalidArgument,
                            SQLITE_MISUSE);
    }
    if (!database_.is_open() || database_.native_handle() == nullptr) {
        return write_result(SqliteSubsystemRegistrationWriteStatus::Unavailable,
                            SQLITE_MISUSE);
    }

    sqlite3* database = database_.native_handle();
    Transaction transaction{database};
    if (!transaction.started()) {
        const int status = transaction.status();
        return write_result(is_busy(status)
            ? SqliteSubsystemRegistrationWriteStatus::Unavailable
            : SqliteSubsystemRegistrationWriteStatus::Error, status);
    }

    Statement update;
    if (!update.prepare(database,
        "UPDATE subsystem_registrations "
        "SET state=?1, registration_revision=?2 "
        "WHERE station_id=?3 AND subsystem_id=?4 "
        "AND registration_revision=?5 AND state<>3;") ||
        sqlite3_bind_int(update.get(), 1, static_cast<int>(next_state)) != SQLITE_OK ||
        !bind_text(update.get(), 2, new_revision) ||
        !bind_text(update.get(), 3, station_id) ||
        !bind_text(update.get(), 4, subsystem_id) ||
        !bind_text(update.get(), 5, expected_revision)) {
        transaction.rollback();
        return write_result(SqliteSubsystemRegistrationWriteStatus::Error,
                            sqlite3_extended_errcode(database));
    }

    const int update_status = update.step();
    const int update_finalize = update.finish();
    if (update_status != SQLITE_DONE || update_finalize != SQLITE_OK) {
        const int status = update_status != SQLITE_DONE
            ? sqlite3_extended_errcode(database) : update_finalize;
        transaction.rollback();
        return write_result(is_busy(status)
            ? SqliteSubsystemRegistrationWriteStatus::Unavailable
            : SqliteSubsystemRegistrationWriteStatus::Error, status);
    }

    if (sqlite3_changes(database) == 1) {
        if (!transaction.commit()) {
            const int status = transaction.status();
            transaction.rollback();
            return write_result(is_busy(status)
                ? SqliteSubsystemRegistrationWriteStatus::Unavailable
                : SqliteSubsystemRegistrationWriteStatus::Error, status);
        }
        return write_result(SqliteSubsystemRegistrationWriteStatus::Updated);
    }

    Statement lookup;
    if (!lookup.prepare(database,
        "SELECT 1 FROM subsystem_registrations WHERE station_id=?1 AND subsystem_id=?2;") ||
        !bind_text(lookup.get(), 1, station_id) ||
        !bind_text(lookup.get(), 2, subsystem_id)) {
        transaction.rollback();
        return write_result(SqliteSubsystemRegistrationWriteStatus::Error,
                            sqlite3_extended_errcode(database));
    }
    const int lookup_status = lookup.step();
    const bool exists = lookup_status == SQLITE_ROW;
    const int lookup_next = exists ? lookup.step() : lookup_status;
    const int lookup_finalize = lookup.finish();
    if ((exists && lookup_next != SQLITE_DONE) ||
        (!exists && lookup_status != SQLITE_DONE) ||
        lookup_finalize != SQLITE_OK) {
        const int status = sqlite3_extended_errcode(database);
        transaction.rollback();
        return write_result(is_busy(status)
            ? SqliteSubsystemRegistrationWriteStatus::Unavailable
            : SqliteSubsystemRegistrationWriteStatus::Error, status);
    }

    if (!transaction.commit()) {
        const int status = transaction.status();
        transaction.rollback();
        return write_result(is_busy(status)
            ? SqliteSubsystemRegistrationWriteStatus::Unavailable
            : SqliteSubsystemRegistrationWriteStatus::Error, status);
    }
    return write_result(exists
        ? SqliteSubsystemRegistrationWriteStatus::RevisionMismatch
        : SqliteSubsystemRegistrationWriteStatus::NotFound);
}

core::SubsystemRegistrationLookupResult
SqliteSubsystemStatusStore::find_subsystem(
    std::string_view station_id, std::string_view subsystem_id) noexcept {
    using Status = core::SubsystemRegistrationLookupStatus;
    if (!valid_identifier(station_id) || !valid_identifier(subsystem_id)) {
        return {Status::Error, {}};
    }
    if (!database_.is_open() || database_.native_handle() == nullptr) {
        return {Status::Unavailable, {}};
    }

    Statement statement;
    if (!statement.prepare(database_.native_handle(),
        "SELECT station_id, subsystem_id, source, registration_revision, state "
        "FROM subsystem_registrations WHERE station_id=?1 AND subsystem_id=?2;") ||
        !bind_text(statement.get(), 1, station_id) ||
        !bind_text(statement.get(), 2, subsystem_id)) {
        return {Status::Error, {}};
    }

    const int query_status = statement.step();
    if (query_status == SQLITE_DONE) {
        return statement.finish() == SQLITE_OK
            ? core::SubsystemRegistrationLookupResult{Status::NotFound, {}}
            : core::SubsystemRegistrationLookupResult{Status::Error, {}};
    }
    if (query_status != SQLITE_ROW) {
        (void)statement.finish();
        const int rc = sqlite3_extended_errcode(database_.native_handle());
        return {is_busy(rc) ? Status::Unavailable : Status::Error, {}};
    }

    std::string station;
    std::string subsystem;
    std::string source;
    std::string revision;
    const int state = sqlite3_column_int(statement.get(), 4);
    const bool copied =
        copy_required_text(statement.get(), 0, station) &&
        copy_required_text(statement.get(), 1, subsystem) &&
        copy_required_text(statement.get(), 2, source) &&
        copy_required_text(statement.get(), 3, revision);
    const int second_status = statement.step();
    const int finalize_status = statement.finish();
    if (!copied || second_status != SQLITE_DONE || finalize_status != SQLITE_OK ||
        state < static_cast<int>(core::SubsystemRegistrationState::Active) ||
        state > static_cast<int>(core::SubsystemRegistrationState::Revoked) ||
        !valid_identifier(station) || !valid_identifier(subsystem) ||
        !valid_identifier(source) || !valid_identifier(revision)) {
        return {Status::Error, {}};
    }

    registration_station_id_ = std::move(station);
    registration_subsystem_id_ = std::move(subsystem);
    registration_source_ = std::move(source);
    registration_revision_ = std::move(revision);
    return {Status::Found,
            {registration_station_id_, registration_subsystem_id_,
             registration_source_, registration_revision_,
             static_cast<core::SubsystemRegistrationState>(state)}};
}

core::EventRepositoryLookupResult
SqliteSubsystemStatusStore::find_by_id(std::string_view event_id) noexcept {
    using Status = core::EventRepositoryLookupStatus;
    if (!valid_identifier(event_id) ||
        !database_.is_open() || database_.native_handle() == nullptr) {
        return {Status::Error, {}};
    }

    Statement statement;
    if (!statement.prepare(database_.native_handle(),
        "SELECT event_id, event_type, source, source_instance, occurred_at, "
        "received_at, source_sequence, operation_id, correlation_id, payload, "
        "quality_status, schema_version, reason_code, reason_text "
        "FROM accepted_events WHERE event_id=?1;") ||
        !bind_text(statement.get(), 1, event_id)) {
        return {Status::Error, {}};
    }

    const int query_status = statement.step();
    if (query_status == SQLITE_DONE) {
        return statement.finish() == SQLITE_OK
            ? core::EventRepositoryLookupResult{Status::NotFound, {}}
            : core::EventRepositoryLookupResult{Status::Error, {}};
    }
    if (query_status != SQLITE_ROW) {
        (void)statement.finish();
        return {Status::Error, {}};
    }

    EventBuffer& buffer = event_buffer_;
    const bool copied =
        copy_required_text(statement.get(), 0, buffer.event_id) &&
        copy_required_text(statement.get(), 1, buffer.event_type) &&
        copy_required_text(statement.get(), 2, buffer.source) &&
        copy_optional_text(statement.get(), 3, buffer.source_instance) &&
        copy_optional_text(statement.get(), 4, buffer.occurred_at) &&
        copy_required_text(statement.get(), 5, buffer.received_at) &&
        parse_sequence(statement.get(), 6, buffer.source_sequence) &&
        copy_optional_text(statement.get(), 7, buffer.operation_id) &&
        copy_optional_text(statement.get(), 8, buffer.correlation_id) &&
        copy_optional_text(statement.get(), 9, buffer.payload) &&
        copy_required_text(statement.get(), 10, buffer.quality_status) &&
        copy_required_text(statement.get(), 11, buffer.schema_version) &&
        copy_optional_text(statement.get(), 12, buffer.reason_code) &&
        copy_optional_text(statement.get(), 13, buffer.reason_text);
    const int second_status = statement.step();
    const int finalize_status = statement.finish();
    if (!copied || second_status != SQLITE_DONE || finalize_status != SQLITE_OK) {
        return {Status::Error, {}};
    }

    cached_event_ = {};
    cached_event_.event_id = buffer.event_id;
    cached_event_.event_type = buffer.event_type;
    cached_event_.source = buffer.source;
    cached_event_.source_instance = view_optional(buffer.source_instance);
    cached_event_.occurred_at = view_optional(buffer.occurred_at);
    cached_event_.received_at = buffer.received_at;
    cached_event_.source_sequence = buffer.source_sequence;
    cached_event_.operation_id = view_optional(buffer.operation_id);
    cached_event_.correlation_id = view_optional(buffer.correlation_id);
    cached_event_.payload = view_optional(buffer.payload);
    cached_event_.quality_status = buffer.quality_status;
    cached_event_.schema_version = buffer.schema_version;
    cached_event_.reason_code = view_optional(buffer.reason_code);
    cached_event_.reason_text = view_optional(buffer.reason_text);
    return {Status::Found, cached_event_};
}

core::SubsystemStatusCommitResult
SqliteSubsystemStatusStore::commit_subsystem_status(
    const core::SubsystemRegistrationRecord& expected_registration,
    const core::DecodedSubsystemStatus& report,
    const core::Event& event,
    const core::SourceCursor& expected_cursor) noexcept {
    using Status = core::SubsystemStatusCommitStatus;

    if (!valid_identifier(expected_registration.station_id) ||
        !valid_identifier(expected_registration.subsystem_id) ||
        !valid_identifier(expected_registration.source) ||
        !valid_identifier(expected_registration.registration_revision) ||
        expected_registration.state != core::SubsystemRegistrationState::Active ||
        !valid_identifier(expected_cursor.source) ||
        !expected_cursor.source_instance.has_value() ||
        !valid_identifier(*expected_cursor.source_instance) ||
        !report_matches_event(report, event) ||
        report.subsystem_id != expected_registration.subsystem_id ||
        event.source != expected_registration.source ||
        expected_cursor.source != event.source ||
        !event.source_instance.has_value() ||
        *event.source_instance != *expected_cursor.source_instance ||
        event.event_type != "SUBSYSTEM_STATUS_UPDATED" ||
        event.quality_status != "VALID" ||
        event.schema_version != "1" ||
        !event.source_sequence.has_value() ||
        !core::validate_event(event, {true, true, false}).valid) {
        return commit_result(Status::Error);
    }
    if (!database_.is_open() || database_.native_handle() == nullptr) {
        return commit_result(Status::Error);
    }

    sqlite3* database = database_.native_handle();
    Transaction transaction{database};
    if (!transaction.started()) return commit_result(Status::Error);

    const core::DuplicateEventResult duplicate = core::check_duplicate_event(event, *this);
    switch (duplicate.status) {
        case core::DuplicateEventStatus::Duplicate:
            transaction.rollback();
            return commit_result(Status::DuplicateEvent);
        case core::DuplicateEventStatus::Conflict:
            transaction.rollback();
            return commit_result(Status::EventConflict);
        case core::DuplicateEventStatus::RepositoryError:
            transaction.rollback();
            return commit_result(Status::Error);
        case core::DuplicateEventStatus::New:
            break;
    }

    // Re-check the exact ACTIVE subsystem registration under the writer lock.
    {
        Statement registration;
        if (!registration.prepare(database,
            "SELECT source, registration_revision, state "
            "FROM subsystem_registrations WHERE station_id=?1 AND subsystem_id=?2;") ||
            !bind_text(registration.get(), 1, expected_registration.station_id) ||
            !bind_text(registration.get(), 2, expected_registration.subsystem_id)) {
            transaction.rollback();
            return commit_result(Status::Error);
        }
        const int status = registration.step();
        if (status == SQLITE_DONE) {
            (void)registration.finish();
            transaction.rollback();
            return commit_result(Status::SubsystemNotRegistered);
        }
        if (status != SQLITE_ROW) {
            (void)registration.finish();
            transaction.rollback();
            return commit_result(Status::Error);
        }

        std::string source;
        std::string revision;
        const bool copied = copy_required_text(registration.get(), 0, source) &&
                            copy_required_text(registration.get(), 1, revision);
        const int state = sqlite3_column_int(registration.get(), 2);
        const int second_status = registration.step();
        const int finalize_status = registration.finish();
        if (!copied || second_status != SQLITE_DONE || finalize_status != SQLITE_OK) {
            transaction.rollback();
            return commit_result(Status::Error);
        }
        if (source != expected_registration.source ||
            revision != expected_registration.registration_revision ||
            state != static_cast<int>(core::SubsystemRegistrationState::Active)) {
            transaction.rollback();
            return commit_result(Status::RegistrationChanged);
        }
    }

    // The transport source may have been disabled after the session gate.
    {
        Statement source_registration;
        if (!source_registration.prepare(database,
            "SELECT 1 FROM source_registrations "
            "WHERE station_id=?1 AND source=?2 AND state=1;") ||
            !bind_text(source_registration.get(), 1, expected_registration.station_id) ||
            !bind_text(source_registration.get(), 2, expected_registration.source)) {
            transaction.rollback();
            return commit_result(Status::Error);
        }
        const int status = source_registration.step();
        const int finalize_status = source_registration.finish();
        if (status != SQLITE_ROW || finalize_status != SQLITE_OK) {
            transaction.rollback();
            return commit_result(status == SQLITE_DONE
                ? Status::RegistrationChanged : Status::Error);
        }
    }

    std::string stored_instance;
    std::optional<std::uint64_t> stored_sequence;
    {
        Statement cursor;
        if (!cursor.prepare(database,
            "SELECT source_instance, last_sequence FROM source_cursors WHERE source=?1;") ||
            !bind_text(cursor.get(), 1, expected_cursor.source)) {
            transaction.rollback();
            return commit_result(Status::Error);
        }
        const int status = cursor.step();
        if (status != SQLITE_ROW) {
            (void)cursor.finish();
            transaction.rollback();
            return commit_result(status == SQLITE_DONE
                ? Status::SourceCursorChanged : Status::Error);
        }
        const bool copied = copy_required_text(cursor.get(), 0, stored_instance) &&
                            parse_sequence(cursor.get(), 1, stored_sequence);
        const int second_status = cursor.step();
        const int finalize_status = cursor.finish();
        if (!copied || second_status != SQLITE_DONE || finalize_status != SQLITE_OK) {
            transaction.rollback();
            return commit_result(Status::Error);
        }
    }
    if (stored_instance != *expected_cursor.source_instance ||
        stored_sequence != expected_cursor.last_sequence ||
        (stored_sequence.has_value() && *event.source_sequence <= *stored_sequence)) {
        transaction.rollback();
        return commit_result(Status::SourceCursorChanged);
    }

    std::array<char, 21U> new_sequence_buffer{};
    std::string_view new_sequence{};
    if (!encode_sequence(*event.source_sequence, new_sequence_buffer, new_sequence)) {
        transaction.rollback();
        return commit_result(Status::Error);
    }
    std::array<char, 21U> old_sequence_buffer{};
    std::string_view old_sequence{};
    if (expected_cursor.last_sequence.has_value() &&
        !encode_sequence(*expected_cursor.last_sequence, old_sequence_buffer, old_sequence)) {
        transaction.rollback();
        return commit_result(Status::Error);
    }

    Statement insert_event;
    if (!insert_event.prepare(database,
        "INSERT INTO accepted_events("
        "event_id,event_type,source,source_instance,occurred_at,received_at,"
        "source_sequence,operation_id,correlation_id,payload,quality_status,"
        "schema_version,reason_code,reason_text) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14);") ||
        !bind_event(insert_event.get(), event, new_sequence)) {
        transaction.rollback();
        return commit_result(Status::Error);
    }
    const int event_status = insert_event.step();
    const int event_finalize = insert_event.finish();
    if (event_status != SQLITE_DONE || event_finalize != SQLITE_OK) {
        transaction.rollback();
        return commit_result(Status::Error);
    }

    Statement insert_history;
    if (!insert_history.prepare(database,
        "INSERT INTO subsystem_status_history("
        "event_id,station_id,subsystem_id,source,status,error_code,severity,description,"
        "reason_text,source_instance,source_sequence,committed_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,"
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'));") ||
        !bind_text(insert_history.get(), 1, event.event_id) ||
        !bind_text(insert_history.get(), 2, expected_registration.station_id) ||
        !bind_text(insert_history.get(), 3, expected_registration.subsystem_id) ||
        !bind_text(insert_history.get(), 4, expected_registration.source) ||
        sqlite3_bind_int(insert_history.get(), 5, static_cast<int>(report.status)) != SQLITE_OK ||
        !bind_text(insert_history.get(), 6, report.error_code) ||
        !bind_text(insert_history.get(), 7, report.severity) ||
        !bind_text(insert_history.get(), 8, report.description) ||
        !(report.reason_text.has_value()
            ? bind_text(insert_history.get(), 9, *report.reason_text)
            : sqlite3_bind_null(insert_history.get(), 9) == SQLITE_OK) ||
        !bind_text(insert_history.get(), 10, *event.source_instance) ||
        !bind_text(insert_history.get(), 11, new_sequence)) {
        transaction.rollback();
        return commit_result(Status::Error);
    }
    const int history_status = insert_history.step();
    const int history_finalize = insert_history.finish();
    if (history_status != SQLITE_DONE || history_finalize != SQLITE_OK) {
        transaction.rollback();
        return commit_result(Status::Error);
    }

    Statement upsert_current;
    if (!upsert_current.prepare(database,
        "INSERT INTO subsystem_status_current(station_id,subsystem_id,last_event_id) "
        "VALUES(?1,?2,?3) ON CONFLICT(station_id,subsystem_id) "
        "DO UPDATE SET last_event_id=excluded.last_event_id;") ||
        !bind_text(upsert_current.get(), 1, expected_registration.station_id) ||
        !bind_text(upsert_current.get(), 2, expected_registration.subsystem_id) ||
        !bind_text(upsert_current.get(), 3, event.event_id)) {
        transaction.rollback();
        return commit_result(Status::Error);
    }
    const int current_status = upsert_current.step();
    const int current_finalize = upsert_current.finish();
    if (current_status != SQLITE_DONE || current_finalize != SQLITE_OK) {
        transaction.rollback();
        return commit_result(Status::Error);
    }

    Statement advance_cursor;
    if (!advance_cursor.prepare(database,
        "UPDATE source_cursors SET last_sequence=?1, "
        "updated_at_utc=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE source=?2 AND source_instance=?3 AND last_sequence IS ?4;") ||
        !bind_text(advance_cursor.get(), 1, new_sequence) ||
        !bind_text(advance_cursor.get(), 2, expected_cursor.source) ||
        !bind_text(advance_cursor.get(), 3, *expected_cursor.source_instance) ||
        !(expected_cursor.last_sequence.has_value()
            ? bind_text(advance_cursor.get(), 4, old_sequence)
            : sqlite3_bind_null(advance_cursor.get(), 4) == SQLITE_OK)) {
        transaction.rollback();
        return commit_result(Status::Error);
    }
    const int cursor_status = advance_cursor.step();
    const int cursor_finalize = advance_cursor.finish();
    if (cursor_status != SQLITE_DONE || cursor_finalize != SQLITE_OK ||
        sqlite3_changes(database) != 1) {
        transaction.rollback();
        return commit_result(Status::SourceCursorChanged);
    }

    if (!transaction.commit()) {
        transaction.rollback();
        return commit_result(Status::Error);
    }
    return commit_result(Status::Committed);
}

}  // namespace dockstation::platform::persistence
