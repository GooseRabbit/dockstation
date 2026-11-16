#include "platform/persistence/sqlite_source_session_store.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include <sqlite3.h>

namespace dockstation::platform::persistence {
namespace {

constexpr std::size_t kMaximumIdentifierBytes = 128U;
constexpr std::size_t kMaximumPeerIdentityBytes = 128U;

class Statement final {
public:
    Statement() noexcept = default;
    ~Statement() {
        if (statement_ != nullptr) {
            (void)sqlite3_finalize(statement_);
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] bool prepare(sqlite3* database, const char* sql) noexcept {
        return sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) ==
               SQLITE_OK;
    }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }
    [[nodiscard]] int step() noexcept { return sqlite3_step(statement_); }

    [[nodiscard]] int finish() noexcept {
        if (statement_ == nullptr) return SQLITE_OK;
        sqlite3_stmt* statement = statement_;
        statement_ = nullptr;
        return sqlite3_finalize(statement);
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
    ~Transaction() {
        if (active_) {
            (void)sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    [[nodiscard]] bool started() const noexcept { return active_; }
    [[nodiscard]] int status() const noexcept { return status_; }

    [[nodiscard]] bool commit() noexcept {
        if (!active_) {
            status_ = SQLITE_MISUSE;
            return false;
        }
        status_ = sqlite3_exec(database_, "COMMIT;", nullptr, nullptr, nullptr);
        if (status_ == SQLITE_OK) active_ = false;
        return status_ == SQLITE_OK;
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

[[nodiscard]] bool is_busy_status(int status) noexcept {
    const int primary = status & 0xFF;
    return primary == SQLITE_BUSY || primary == SQLITE_LOCKED;
}

[[nodiscard]] bool valid_text_identifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumIdentifierBytes ||
        value.find(static_cast<char>(0)) != std::string_view::npos) {
        return false;
    }
    bool non_whitespace = false;
    for (const unsigned char ch : value) {
        if (ch < 0x20U || ch == 0x7FU) return false;
        if (ch != ' ' && ch != '\t' && ch != '\n' &&
            ch != '\r' && ch != '\f' && ch != '\v') {
            non_whitespace = true;
        }
    }
    return non_whitespace;
}

[[nodiscard]] bool valid_peer_identity(std::string_view identity) noexcept {
    return !identity.empty() && identity.size() <= kMaximumPeerIdentityBytes;
}

[[nodiscard]] bool bind_text(
    sqlite3_stmt* statement, int index, std::string_view value) noexcept {
    return value.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
           sqlite3_bind_text(statement, index, value.data(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

[[nodiscard]] bool bind_blob(
    sqlite3_stmt* statement, int index, std::string_view value) noexcept {
    return value.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
           sqlite3_bind_blob(statement, index, value.data(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

[[nodiscard]] bool copy_text_column(
    sqlite3_stmt* statement, int column, std::string& output) {
    const auto* bytes = sqlite3_column_text(statement, column);
    const int length = sqlite3_column_bytes(statement, column);
    if (bytes == nullptr || length <= 0 ||
        std::memchr(bytes, 0, static_cast<std::size_t>(length)) != nullptr) {
        return false;
    }
    output.assign(reinterpret_cast<const char*>(bytes),
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
    const auto encoded = std::to_chars(canonical.data(),
                                       canonical.data() + canonical.size(),
                                       value, 10);
    if (encoded.ec != std::errc{} ||
        std::string_view{canonical.data(),
            static_cast<std::size_t>(encoded.ptr - canonical.data())} !=
        std::string_view{begin, static_cast<std::size_t>(length)}) {
        return false;
    }
    output = value;
    return true;
}

[[nodiscard]] bool is_known_registration_state(
    core::SourceRegistrationState state) noexcept {
    return state == core::SourceRegistrationState::Active ||
           state == core::SourceRegistrationState::Disabled ||
           state == core::SourceRegistrationState::Revoked;
}

[[nodiscard]] SqliteSourceRegistrationWriteResult write_result(
    SqliteSourceRegistrationWriteStatus status, int sqlite_status = SQLITE_OK) noexcept {
    return {status, sqlite_status};
}

[[nodiscard]] core::SourceSessionConfirmationResult confirmation_failure(
    core::SourceSessionConfirmationStatus status) noexcept {
    return {status, {}};
}

}  // namespace

SqliteSourceSessionStore::SqliteSourceSessionStore(
    SqliteDatabase& database) noexcept
    : database_(database) {}

SqliteSourceRegistrationWriteResult
SqliteSourceSessionStore::create_source_registration(
    std::string_view station_id,
    std::string_view source,
    std::string_view registration_revision,
    std::string_view peer_identity) noexcept {
    if (!valid_text_identifier(station_id) ||
        !valid_text_identifier(source) ||
        !valid_text_identifier(registration_revision) ||
        !valid_peer_identity(peer_identity)) {
        return write_result(SqliteSourceRegistrationWriteStatus::InvalidArgument,
                            SQLITE_MISUSE);
    }
    if (!database_.is_open() || database_.native_handle() == nullptr) {
        return write_result(SqliteSourceRegistrationWriteStatus::Unavailable,
                            SQLITE_MISUSE);
    }

    sqlite3* database = database_.native_handle();
    Transaction transaction{database};
    if (!transaction.started()) {
        const int status = transaction.status();
        return write_result(is_busy_status(status)
            ? SqliteSourceRegistrationWriteStatus::Unavailable
            : SqliteSourceRegistrationWriteStatus::Error, status);
    }

    Statement statement;
    if (!statement.prepare(database,
        "INSERT INTO source_registrations("
        "peer_identity, station_id, source, registration_revision, state) "
        "VALUES(?1, ?2, ?3, ?4, 1);") ||
        !bind_blob(statement.get(), 1, peer_identity) ||
        !bind_text(statement.get(), 2, station_id) ||
        !bind_text(statement.get(), 3, source) ||
        !bind_text(statement.get(), 4, registration_revision)) {
        transaction.rollback();
        return write_result(SqliteSourceRegistrationWriteStatus::Error,
                            sqlite3_extended_errcode(database));
    }

    const int step_status = statement.step();
    const int finalize_status = statement.finish();
    if (step_status != SQLITE_DONE || finalize_status != SQLITE_OK) {
        const int status = step_status != SQLITE_DONE
            ? sqlite3_extended_errcode(database) : finalize_status;
        transaction.rollback();
        if ((status & 0xFF) == SQLITE_CONSTRAINT) {
            return write_result(SqliteSourceRegistrationWriteStatus::Conflict, status);
        }
        return write_result(is_busy_status(status)
            ? SqliteSourceRegistrationWriteStatus::Unavailable
            : SqliteSourceRegistrationWriteStatus::Error, status);
    }

    if (!transaction.commit()) {
        const int status = transaction.status();
        transaction.rollback();
        return write_result(is_busy_status(status)
            ? SqliteSourceRegistrationWriteStatus::Unavailable
            : SqliteSourceRegistrationWriteStatus::Error, status);
    }
    return write_result(SqliteSourceRegistrationWriteStatus::Created);
}

SqliteSourceRegistrationWriteResult
SqliteSourceSessionStore::change_source_state(
    std::string_view source,
    std::string_view expected_revision,
    std::string_view new_revision,
    core::SourceRegistrationState next_state) noexcept {
    if (!valid_text_identifier(source) ||
        !valid_text_identifier(expected_revision) ||
        !valid_text_identifier(new_revision) ||
        expected_revision == new_revision ||
        !is_known_registration_state(next_state)) {
        return write_result(SqliteSourceRegistrationWriteStatus::InvalidArgument,
                            SQLITE_MISUSE);
    }
    if (!database_.is_open() || database_.native_handle() == nullptr) {
        return write_result(SqliteSourceRegistrationWriteStatus::Unavailable,
                            SQLITE_MISUSE);
    }

    sqlite3* database = database_.native_handle();
    Transaction transaction{database};
    if (!transaction.started()) {
        const int status = transaction.status();
        return write_result(is_busy_status(status)
            ? SqliteSourceRegistrationWriteStatus::Unavailable
            : SqliteSourceRegistrationWriteStatus::Error, status);
    }

    Statement statement;
    if (!statement.prepare(database,
        "UPDATE source_registrations "
        "SET state=?1, registration_revision=?2 "
        "WHERE source=?3 AND registration_revision=?4 AND state<>3;") ||
        sqlite3_bind_int(statement.get(), 1, static_cast<int>(next_state)) != SQLITE_OK ||
        !bind_text(statement.get(), 2, new_revision) ||
        !bind_text(statement.get(), 3, source) ||
        !bind_text(statement.get(), 4, expected_revision)) {
        transaction.rollback();
        return write_result(SqliteSourceRegistrationWriteStatus::Error,
                            sqlite3_extended_errcode(database));
    }

    const int step_status = statement.step();
    const int finalize_status = statement.finish();
    if (step_status != SQLITE_DONE || finalize_status != SQLITE_OK) {
        const int status = step_status != SQLITE_DONE
            ? sqlite3_extended_errcode(database) : finalize_status;
        transaction.rollback();
        return write_result(is_busy_status(status)
            ? SqliteSourceRegistrationWriteStatus::Unavailable
            : SqliteSourceRegistrationWriteStatus::Error, status);
    }

    if (sqlite3_changes(database) == 1) {
        if (!transaction.commit()) {
            const int status = transaction.status();
            transaction.rollback();
            return write_result(is_busy_status(status)
                ? SqliteSourceRegistrationWriteStatus::Unavailable
                : SqliteSourceRegistrationWriteStatus::Error, status);
        }
        return write_result(SqliteSourceRegistrationWriteStatus::Updated);
    }

    Statement lookup;
    if (!lookup.prepare(database,
        "SELECT 1 FROM source_registrations WHERE source=?1;") ||
        !bind_text(lookup.get(), 1, source)) {
        transaction.rollback();
        return write_result(SqliteSourceRegistrationWriteStatus::Error,
                            sqlite3_extended_errcode(database));
    }
    const int lookup_status = lookup.step();
    const bool exists = lookup_status == SQLITE_ROW;
    const int second_status = exists ? lookup.step() : lookup_status;
    const int lookup_finalize = lookup.finish();
    if ((exists && second_status != SQLITE_DONE) ||
        (!exists && lookup_status != SQLITE_DONE) ||
        lookup_finalize != SQLITE_OK) {
        const int status = sqlite3_extended_errcode(database);
        transaction.rollback();
        return write_result(is_busy_status(status)
            ? SqliteSourceRegistrationWriteStatus::Unavailable
            : SqliteSourceRegistrationWriteStatus::Error, status);
    }

    if (!transaction.commit()) {
        const int status = transaction.status();
        transaction.rollback();
        return write_result(is_busy_status(status)
            ? SqliteSourceRegistrationWriteStatus::Unavailable
            : SqliteSourceRegistrationWriteStatus::Error, status);
    }
    return write_result(exists
        ? SqliteSourceRegistrationWriteStatus::RevisionMismatch
        : SqliteSourceRegistrationWriteStatus::NotFound);
}

core::SourceRegistrationLookupResult
SqliteSourceSessionStore::find_by_peer_identity(
    std::string_view peer_identity_evidence) noexcept {
    using core::SourceRegistrationLookupStatus;
    if (!valid_peer_identity(peer_identity_evidence)) {
        return {SourceRegistrationLookupStatus::Error, {}};
    }
    if (!database_.is_open() || database_.native_handle() == nullptr) {
        return {SourceRegistrationLookupStatus::Unavailable, {}};
    }

    sqlite3* database = database_.native_handle();
    Statement statement;
    if (!statement.prepare(database,
        "SELECT station_id, source, registration_revision, state "
        "FROM source_registrations WHERE peer_identity=?1;") ||
        !bind_blob(statement.get(), 1, peer_identity_evidence)) {
        return {SourceRegistrationLookupStatus::Error, {}};
    }

    const int step_status = statement.step();
    if (step_status == SQLITE_DONE) {
        return statement.finish() == SQLITE_OK
            ? core::SourceRegistrationLookupResult{
                  SourceRegistrationLookupStatus::NotFound, {}}
            : core::SourceRegistrationLookupResult{
                  SourceRegistrationLookupStatus::Error, {}};
    }
    if (step_status != SQLITE_ROW) {
        const int status = sqlite3_extended_errcode(database);
        (void)statement.finish();
        return {is_busy_status(status) ? SourceRegistrationLookupStatus::Unavailable
                                       : SourceRegistrationLookupStatus::Error, {}};
    }

    std::string station_id;
    std::string source;
    std::string revision;
    const int state_value = sqlite3_column_int(statement.get(), 3);
    const bool copied =
        copy_text_column(statement.get(), 0, station_id) &&
        copy_text_column(statement.get(), 1, source) &&
        copy_text_column(statement.get(), 2, revision);
    const int second_status = statement.step();
    const int finalize_status = statement.finish();
    if (!copied || second_status != SQLITE_DONE || finalize_status != SQLITE_OK ||
        state_value < static_cast<int>(core::SourceRegistrationState::Active) ||
        state_value > static_cast<int>(core::SourceRegistrationState::Revoked)) {
        return {SourceRegistrationLookupStatus::Error, {}};
    }

    registration_station_id_ = std::move(station_id);
    registration_source_ = std::move(source);
    registration_revision_ = std::move(revision);
    const auto state = static_cast<core::SourceRegistrationState>(state_value);
    if (!valid_text_identifier(registration_station_id_) ||
        !valid_text_identifier(registration_source_) ||
        !valid_text_identifier(registration_revision_)) {
        return {SourceRegistrationLookupStatus::Error, {}};
    }

    return {SourceRegistrationLookupStatus::Found,
            {registration_station_id_, registration_source_,
             registration_revision_, state}};
}

core::SourceCursorReadResult
SqliteSourceSessionStore::read_source_cursor(
    std::string_view source) noexcept {
    using core::SourceCursorReadStatus;
    if (!valid_text_identifier(source)) {
        return {SourceCursorReadStatus::Error, {}};
    }
    if (!database_.is_open() || database_.native_handle() == nullptr) {
        return {SourceCursorReadStatus::Error, {}};
    }

    sqlite3* database = database_.native_handle();
    Statement statement;
    if (!statement.prepare(database,
        "SELECT source_instance, last_sequence "
        "FROM source_cursors WHERE source=?1;") ||
        !bind_text(statement.get(), 1, source)) {
        return {SourceCursorReadStatus::Error, {}};
    }

    const int step_status = statement.step();
    if (step_status == SQLITE_DONE) {
        return statement.finish() == SQLITE_OK
            ? core::SourceCursorReadResult{SourceCursorReadStatus::NotFound, {}}
            : core::SourceCursorReadResult{SourceCursorReadStatus::Error, {}};
    }
    if (step_status != SQLITE_ROW) {
        (void)statement.finish();
        return {SourceCursorReadStatus::Error, {}};
    }

    std::string instance;
    std::optional<std::uint64_t> sequence;
    const bool copied = copy_text_column(statement.get(), 0, instance) &&
                        parse_sequence(statement.get(), 1, sequence);
    const int second_status = statement.step();
    const int finalize_status = statement.finish();
    if (!copied || second_status != SQLITE_DONE ||
        finalize_status != SQLITE_OK || !valid_text_identifier(instance)) {
        return {SourceCursorReadStatus::Error, {}};
    }

    cursor_source_.assign(source);
    cursor_instance_ = std::move(instance);
    return {SourceCursorReadStatus::Found,
            {cursor_source_, cursor_instance_, sequence}};
}

core::SourceSessionConfirmationResult
SqliteSourceSessionStore::confirm_source_session(
    const core::SourceCursorReadResult& expected_cursor,
    const core::VerifiedSourceSession& verified_session) noexcept {
    using core::SourceCursorReadStatus;
    using core::SourceSessionConfirmationStatus;

    if (!core::is_valid_verified_source_session(verified_session) ||
        !core::is_valid_source_cursor_read_result(verified_session.source, expected_cursor) ||
        (expected_cursor.status != SourceCursorReadStatus::Found &&
         expected_cursor.status != SourceCursorReadStatus::NotFound)) {
        return confirmation_failure(SourceSessionConfirmationStatus::Error);
    }
    if (!database_.is_open() || database_.native_handle() == nullptr) {
        return confirmation_failure(SourceSessionConfirmationStatus::Error);
    }

    sqlite3* database = database_.native_handle();
    Transaction transaction{database};
    if (!transaction.started()) {
        return confirmation_failure(SourceSessionConfirmationStatus::Error);
    }

    // Re-check the ACTIVE registration and revision under the writer lock.
    {
        Statement registration;
        if (!registration.prepare(database,
            "SELECT registration_revision, state "
            "FROM source_registrations WHERE source=?1;") ||
            !bind_text(registration.get(), 1, verified_session.source)) {
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::Error);
        }

        const int status = registration.step();
        if (status == SQLITE_DONE) {
            (void)registration.finish();
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::RegistrationChanged);
        }
        if (status != SQLITE_ROW) {
            (void)registration.finish();
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::Error);
        }

        std::string current_revision;
        const bool copied = copy_text_column(registration.get(), 0, current_revision);
        const int current_state = sqlite3_column_int(registration.get(), 1);
        const int second_status = registration.step();
        const int finalize_status = registration.finish();
        if (!copied || second_status != SQLITE_DONE || finalize_status != SQLITE_OK) {
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::Error);
        }
        if (current_state != static_cast<int>(core::SourceRegistrationState::Active) ||
            current_revision != verified_session.registration_revision) {
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::RegistrationChanged);
        }
    }

    bool cursor_exists = false;
    std::string current_instance;
    std::optional<std::uint64_t> current_sequence;
    {
        Statement cursor;
        if (!cursor.prepare(database,
            "SELECT source_instance, last_sequence "
            "FROM source_cursors WHERE source=?1;") ||
            !bind_text(cursor.get(), 1, verified_session.source)) {
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::Error);
        }

        const int status = cursor.step();
        if (status == SQLITE_ROW) {
            cursor_exists = true;
            const bool copied =
                copy_text_column(cursor.get(), 0, current_instance) &&
                parse_sequence(cursor.get(), 1, current_sequence);
            const int second_status = cursor.step();
            const int finalize_status = cursor.finish();
            if (!copied || second_status != SQLITE_DONE ||
                finalize_status != SQLITE_OK || !valid_text_identifier(current_instance)) {
                transaction.rollback();
                return confirmation_failure(SourceSessionConfirmationStatus::Error);
            }
        } else if (status == SQLITE_DONE) {
            if (cursor.finish() != SQLITE_OK) {
                transaction.rollback();
                return confirmation_failure(SourceSessionConfirmationStatus::Error);
            }
        } else {
            (void)cursor.finish();
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::Error);
        }
    }

    bool expected_matches = false;
    if (expected_cursor.status == SourceCursorReadStatus::NotFound) {
        expected_matches = !cursor_exists;
    } else if (expected_cursor.status == SourceCursorReadStatus::Found &&
               cursor_exists && expected_cursor.cursor.source_instance.has_value()) {
        expected_matches =
            current_instance == *expected_cursor.cursor.source_instance &&
            current_sequence == expected_cursor.cursor.last_sequence;
    }
    if (!expected_matches) {
        transaction.rollback();
        return confirmation_failure(SourceSessionConfirmationStatus::CursorChanged);
    }

    if (cursor_exists && current_instance == verified_session.source_instance) {
        if (!transaction.commit()) {
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::Error);
        }
        return {SourceSessionConfirmationStatus::AlreadyCurrent, expected_cursor.cursor};
    }

    if (!cursor_exists) {
        Statement insert;
        if (!insert.prepare(database,
            "INSERT INTO source_cursors("
            "source, source_instance, last_sequence, updated_at_utc) "
            "VALUES(?1, ?2, NULL, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));") ||
            !bind_text(insert.get(), 1, verified_session.source) ||
            !bind_text(insert.get(), 2, verified_session.source_instance)) {
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::Error);
        }
        const int write_status = insert.step();
        const int finalize_status = insert.finish();
        if (write_status != SQLITE_DONE || finalize_status != SQLITE_OK ||
            sqlite3_changes(database) != 1) {
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::Error);
        }
    } else {
        Statement update;
        if (!update.prepare(database,
            "UPDATE source_cursors "
            "SET source_instance=?1, last_sequence=NULL, "
            "updated_at_utc=strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
            "WHERE source=?2;") ||
            !bind_text(update.get(), 1, verified_session.source_instance) ||
            !bind_text(update.get(), 2, verified_session.source)) {
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::Error);
        }
        const int write_status = update.step();
        const int finalize_status = update.finish();
        if (write_status != SQLITE_DONE || finalize_status != SQLITE_OK ||
            sqlite3_changes(database) != 1) {
            transaction.rollback();
            return confirmation_failure(SourceSessionConfirmationStatus::Error);
        }
    }

    if (!transaction.commit()) {
        transaction.rollback();
        return confirmation_failure(SourceSessionConfirmationStatus::Error);
    }

    // Keep the reader's expected-cursor buffers unchanged until the
    // coordinator validates this result against that original snapshot.
    confirmation_source_.assign(verified_session.source);
    confirmation_instance_.assign(verified_session.source_instance);
    return {SourceSessionConfirmationStatus::Confirmed,
            {confirmation_source_, confirmation_instance_, std::nullopt}};
}

}  // namespace dockstation::platform::persistence
