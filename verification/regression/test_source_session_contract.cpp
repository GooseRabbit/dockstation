#include "core/event_processing.hpp"
#include "core/source_session.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace dockstation::core;

int failures = 0;
int checks = 0;

void expect(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

SourceSessionEvidence evidence() {
    return {"esp32/main", "boot-new", "trusted-peer", "fresh-proof"};
}
VerifiedSourceSession verified() {
    return {"esp32/main", "boot-new", "reg-7"};
}

class FakeVerifier final : public SourceSessionVerifier {
public:
    SourceSessionVerificationResult result{
        SourceSessionVerificationStatus::Verified, verified()};
    SourceSessionVerificationResult verify_source_session(
        const SourceSessionEvidence& input) noexcept override {
        ++calls;
        if (input.peer_identity_evidence != "trusted-peer" ||
            input.session_proof != "fresh-proof") {
            return {SourceSessionVerificationStatus::Rejected, {}};
        }
        return result;
    }
    int calls{0};
};

class FakeReader final : public SourceCursorReader {
public:
    SourceCursorReadStatus status{SourceCursorReadStatus::NotFound};
    std::string source{"esp32/main"};
    std::optional<std::string> instance;
    std::optional<std::uint64_t> sequence;
    bool malformed{false};
    SourceCursorReadResult read_source_cursor(
        std::string_view key) noexcept override {
        ++calls;
        if (malformed) {
            return {SourceCursorReadStatus::Found,
                    {"esp32/other", std::nullopt, std::nullopt}};
        }
        if (status != SourceCursorReadStatus::Found) {
            return {status, {}};
        }
        if (key != source) {
            return {SourceCursorReadStatus::NotFound, {}};
        }
        std::optional<std::string_view> view;
        if (instance) view = *instance;
        return {SourceCursorReadStatus::Found, {source, view, sequence}};
    }
    int calls{0};
};

struct OwnedCursor {
    std::string source;
    std::optional<std::string> instance;
    std::optional<std::uint64_t> sequence;
};

bool same_cursor(const SourceCursor& a, const SourceCursor& b) noexcept {
    return a.source == b.source &&
           a.source_instance.has_value() == b.source_instance.has_value() &&
           (!a.source_instance || *a.source_instance == *b.source_instance) &&
           a.last_sequence == b.last_sequence;
}

class FakeRepository final : public SourceSessionRepository {
public:
    std::optional<OwnedCursor> current;
    std::string revision{"reg-7"};
    bool active{true};
    bool change_cursor_before_compare{false};
    std::optional<SourceSessionConfirmationResult> forced;
    int calls{0};

    SourceSessionConfirmationResult confirm_source_session(
        const SourceCursorReadResult& expected,
        const VerifiedSourceSession& session) noexcept override {
        ++calls;
        if (forced) return *forced;

        if (change_cursor_before_compare) {
            change_cursor_before_compare = false;
            current = OwnedCursor{"esp32/main", std::string{"racing-boot"}, 2U};
        }
        if (!active || revision != session.registration_revision) {
            return {SourceSessionConfirmationStatus::RegistrationChanged, {}};
        }
        if (!matches(expected)) {
            return {SourceSessionConfirmationStatus::CursorChanged, {}};
        }
        if (current && current->instance &&
            *current->instance == session.source_instance) {
            return {SourceSessionConfirmationStatus::AlreadyCurrent, view()};
        }
        current = OwnedCursor{std::string{session.source},
                              std::string{session.source_instance}, std::nullopt};
        return {SourceSessionConfirmationStatus::Confirmed, view()};
    }

    void set(std::string source, std::optional<std::string> instance,
             std::optional<std::uint64_t> sequence) {
        current = OwnedCursor{std::move(source), std::move(instance), sequence};
    }

    SourceCursorReadResult result() const noexcept {
        if (!current) return {SourceCursorReadStatus::NotFound, {}};
        std::optional<std::string_view> instance_view;
        if (current->instance) instance_view = *current->instance;
        return {SourceCursorReadStatus::Found,
                {current->source, instance_view, current->sequence}};
    }

private:
    bool matches(const SourceCursorReadResult& expected) const noexcept {
        if (expected.status == SourceCursorReadStatus::NotFound) return !current;
        if (expected.status != SourceCursorReadStatus::Found || !current) return false;
        return same_cursor(expected.cursor, result().cursor);
    }
    SourceCursor view() const noexcept {
        std::optional<std::string_view> instance_view;
        if (current->instance) instance_view = *current->instance;
        return {current->source, instance_view, current->sequence};
    }
};

enum class Flow {
    Confirmed, AlreadyCurrent, Unknown, Rejected, Unavailable, VerifyError,
    ReadError, CursorChanged, RegistrationChanged, CommitError
};

// Test-only reference orchestration, not production composition-root code.
Flow run_flow(SourceSessionEvidence input, SourceSessionVerifier& verifier,
              SourceCursorReader& reader,
              SourceSessionRepository& repository) noexcept {
    if (!is_valid_source_session_evidence(input)) return Flow::Rejected;
    const auto checked = verifier.verify_source_session(input);
    if (!is_valid_source_session_verification_result(input, checked)) {
        return Flow::VerifyError;
    }
    switch (checked.status) {
        case SourceSessionVerificationStatus::UnknownSource: return Flow::Unknown;
        case SourceSessionVerificationStatus::Rejected: return Flow::Rejected;
        case SourceSessionVerificationStatus::Unavailable: return Flow::Unavailable;
        case SourceSessionVerificationStatus::Error: return Flow::VerifyError;
        case SourceSessionVerificationStatus::Verified: break;
    }
    const auto expected = reader.read_source_cursor(checked.session.source);
    if (!is_valid_source_cursor_read_result(checked.session.source, expected) ||
        expected.status == SourceCursorReadStatus::Error) return Flow::ReadError;
    const auto committed = repository.confirm_source_session(expected, checked.session);
    if (!is_valid_source_session_confirmation_result(expected, checked.session,
                                                      committed)) return Flow::CommitError;
    switch (committed.status) {
        case SourceSessionConfirmationStatus::Confirmed: return Flow::Confirmed;
        case SourceSessionConfirmationStatus::AlreadyCurrent: return Flow::AlreadyCurrent;
        case SourceSessionConfirmationStatus::CursorChanged: return Flow::CursorChanged;
        case SourceSessionConfirmationStatus::RegistrationChanged:
            return Flow::RegistrationChanged;
        case SourceSessionConfirmationStatus::Error: return Flow::CommitError;
    }
    return Flow::CommitError;
}

void ssr_01_unknown_source() {
    FakeVerifier v; v.result = {SourceSessionVerificationStatus::UnknownSource, {}};
    FakeReader r; FakeRepository p;
    expect(run_flow(evidence(), v, r, p) == Flow::Unknown, "SSR-01 unknown source");
    expect(p.calls == 0 && !p.current, "SSR-01 no cursor for unknown source");
}
void ssr_02_disabled_revoked_source() {
    FakeVerifier v; v.result = {SourceSessionVerificationStatus::Rejected, {}};
    FakeReader r; FakeRepository p;
    expect(run_flow(evidence(), v, r, p) == Flow::Rejected, "SSR-02 disabled/revoked rejected");
    expect(p.calls == 0, "SSR-02 repository not called");
}
void ssr_03_peer_mismatch() {
    FakeVerifier v; FakeReader r; FakeRepository p;
    auto e = evidence(); e.peer_identity_evidence = "other-peer";
    expect(run_flow(e, v, r, p) == Flow::Rejected, "SSR-03 peer identity mismatch");
    expect(p.calls == 0, "SSR-03 no cursor mutation");
}
void ssr_04_replayed_proof() {
    FakeVerifier v; FakeReader r; FakeRepository p;
    auto e = evidence(); e.session_proof = "replayed";
    expect(run_flow(e, v, r, p) == Flow::Rejected, "SSR-04 replayed proof rejected");
    expect(p.calls == 0, "SSR-04 replay cannot reach repository");
}
void ssr_05_verifier_errors() {
    FakeReader r; FakeRepository p;
    FakeVerifier unavailable;
    unavailable.result = {SourceSessionVerificationStatus::Unavailable, {}};
    expect(run_flow(evidence(), unavailable, r, p) == Flow::Unavailable,
           "SSR-05 Unavailable fails closed");
    FakeVerifier error; error.result = {SourceSessionVerificationStatus::Error, {}};
    expect(run_flow(evidence(), error, r, p) == Flow::VerifyError,
           "SSR-05 Error fails closed");
    FakeVerifier unknown;
    unknown.result.status = static_cast<SourceSessionVerificationStatus>(255U);
    expect(run_flow(evidence(), unknown, r, p) == Flow::VerifyError,
           "SSR-05 unknown status fails closed");
    expect(p.calls == 0, "SSR-05 no failed verifier result reaches repository");
}
void ssr_06_first_session_created() {
    FakeVerifier v; FakeReader r; FakeRepository p;
    expect(run_flow(evidence(), v, r, p) == Flow::Confirmed, "SSR-06 first session confirmed");
    const auto stored = p.result();
    expect(stored.status == SourceCursorReadStatus::Found &&
           stored.cursor.source == "esp32/main" &&
           stored.cursor.source_instance == std::optional<std::string_view>{"boot-new"} &&
           !stored.cursor.last_sequence, "SSR-06 exact initial cursor stored without sequence");
}
void ssr_07_unverified_first_session() {
    FakeVerifier v; v.result = {SourceSessionVerificationStatus::Rejected, {}};
    FakeReader r; FakeRepository p;
    expect(run_flow(evidence(), v, r, p) == Flow::Rejected, "SSR-07 unverified first session blocked");
    expect(!p.current && p.calls == 0, "SSR-07 no initial cursor created");
}
void ssr_08_same_session_preserves_sequence() {
    FakeVerifier v; FakeReader r; FakeRepository p;
    r.status = SourceCursorReadStatus::Found; r.instance = "boot-new"; r.sequence = 73U;
    p.set("esp32/main", std::string{"boot-new"}, 73U);
    expect(run_flow(evidence(), v, r, p) == Flow::AlreadyCurrent, "SSR-08 same instance is AlreadyCurrent");
    expect(p.result().cursor.last_sequence == std::optional<std::uint64_t>{73U},
           "SSR-08 last_sequence is preserved");
}
void ssr_09_new_session_resets_sequence() {
    FakeVerifier v; FakeReader r; FakeRepository p;
    r.status = SourceCursorReadStatus::Found; r.instance = "boot-old"; r.sequence = 73U;
    p.set("esp32/main", std::string{"boot-old"}, 73U);
    expect(run_flow(evidence(), v, r, p) == Flow::Confirmed, "SSR-09 new verified instance confirmed");
    expect(p.result().cursor.source_instance == std::optional<std::string_view>{"boot-new"} &&
           !p.result().cursor.last_sequence, "SSR-09 sequence cleared with instance update");
}
void ssr_10_reader_failures() {
    FakeVerifier v; FakeReader r; FakeRepository p;
    r.status = SourceCursorReadStatus::Error;
    expect(run_flow(evidence(), v, r, p) == Flow::ReadError, "SSR-10 reader Error not NotFound");
    r.status = SourceCursorReadStatus::Found; r.malformed = true;
    expect(run_flow(evidence(), v, r, p) == Flow::ReadError, "SSR-10 malformed Found rejected");
    r.status = static_cast<SourceCursorReadStatus>(255U);
    expect(run_flow(evidence(), v, r, p) == Flow::ReadError,
           "SSR-10 unknown reader status fails closed");
    expect(p.calls == 0, "SSR-10 invalid read prevents commit");
}
void ssr_11_cursor_race() {
    FakeVerifier v; FakeReader r; FakeRepository p;
    p.change_cursor_before_compare = true;
    expect(run_flow(evidence(), v, r, p) == Flow::CursorChanged, "SSR-11 cursor race returns conflict");
    expect(p.current && p.current->instance == std::optional<std::string>{"racing-boot"},
           "SSR-11 newer cursor is not overwritten");
}
void ssr_12_two_sessions_compete() {
    FakeRepository p;
    const SourceCursorReadResult expected{SourceCursorReadStatus::NotFound, {}};
    const VerifiedSourceSession a{"esp32/main", "boot-a", "reg-7"};
    const VerifiedSourceSession b{"esp32/main", "boot-b", "reg-7"};
    const auto first = p.confirm_source_session(expected, a);
    const auto second = p.confirm_source_session(expected, b);
    expect(first.status == SourceSessionConfirmationStatus::Confirmed,
           "SSR-12 first competing session wins");
    expect(second.status == SourceSessionConfirmationStatus::CursorChanged,
           "SSR-12 stale second confirmation loses compare-and-set");
    expect(p.current && p.current->instance == std::optional<std::string>{"boot-a"},
           "SSR-12 winner remains stored");
}
void ssr_13_registration_changed() {
    FakeVerifier v; FakeReader r; FakeRepository p; p.revision = "reg-8";
    expect(run_flow(evidence(), v, r, p) == Flow::RegistrationChanged,
           "SSR-13 registration revision race blocks commit");
    expect(!p.current, "SSR-13 changed registry leaves cursor unchanged");
    p.revision = "reg-7"; p.active = false;
    expect(run_flow(evidence(), v, r, p) == Flow::RegistrationChanged,
           "SSR-13 disabled/revoked registration blocks commit");
}
bool event_matches_session(const Event& event,
                           const VerifiedSourceSession& session) noexcept {
    return event.source == session.source && event.source_instance &&
           *event.source_instance == session.source_instance;
}
void ssr_14_event_context_binding() {
    Event event{}; event.source = "esp32/main"; event.source_instance = "boot-new";
    expect(event_matches_session(event, verified()), "SSR-14 matching event context");
    event.source_instance = "boot-old";
    expect(!event_matches_session(event, verified()), "SSR-14 wrong instance rejected before F13");
    event.source_instance = "boot-new"; event.source = "esp32/other";
    expect(!event_matches_session(event, verified()), "SSR-14 wrong source rejected before F13");
}
void ssr_15_delayed_old_session() {
    Event event{}; event.source = "esp32/main"; event.source_instance = "boot-old";
    event.source_sequence = 90U;
    const SourceCursor cursor{"esp32/main", std::string_view{"boot-new"}, std::nullopt};
    const auto result = check_event_freshness(event, cursor);
    expect(result.status == EventFreshnessStatus::Unordered &&
           result.reason == EventFreshnessReason::SourceInstanceMismatch,
           "SSR-15 delayed old-session event stays unordered");
}
void ssr_16_missing_sequence() {
    Event event{}; event.source = "esp32/main"; event.source_instance = "boot-new";
    const SourceCursor cursor{"esp32/main", std::string_view{"boot-new"}, std::nullopt};
    const auto result = check_event_freshness(event, cursor);
    expect(result.status == EventFreshnessStatus::Unordered &&
           result.reason == EventFreshnessReason::SequenceMissing,
           "SSR-16 missing sequence has no timestamp fallback");
}
void ssr_17_bad_repository_results() {
    FakeVerifier v; FakeReader r; FakeRepository p;
    p.forced = SourceSessionConfirmationResult{SourceSessionConfirmationStatus::Error, {}};
    expect(run_flow(evidence(), v, r, p) == Flow::CommitError, "SSR-17 repository Error blocks");
    p.forced = SourceSessionConfirmationResult{
        static_cast<SourceSessionConfirmationStatus>(255U), {}};
    expect(run_flow(evidence(), v, r, p) == Flow::CommitError, "SSR-17 unknown result status blocks");
    p.forced = SourceSessionConfirmationResult{
        SourceSessionConfirmationStatus::Confirmed,
        {"esp32/main", std::string_view{"boot-new"}, 2U}};
    expect(run_flow(evidence(), v, r, p) == Flow::CommitError,
           "SSR-17 contradictory successful cursor is rejected");
}
void ssr_18_no_session_identity_or_sequence() {
    auto e = evidence(); e.source_instance = {};
    expect(!is_valid_source_session_evidence(e),
           "SSR-18 session verifier requires a non-empty source_instance");
    Event event{}; event.source = "legacy/sensor";
    const SourceCursor cursor{"legacy/sensor", std::nullopt, std::nullopt};
    const auto result = check_event_freshness(event, cursor);
    expect(result.status == EventFreshnessStatus::Unordered &&
           result.reason == EventFreshnessReason::SequenceMissing,
           "SSR-18 no sequence remains unordered");
}

}  // namespace

int main() {
    ssr_01_unknown_source();
    ssr_02_disabled_revoked_source();
    ssr_03_peer_mismatch();
    ssr_04_replayed_proof();
    ssr_05_verifier_errors();
    ssr_06_first_session_created();
    ssr_07_unverified_first_session();
    ssr_08_same_session_preserves_sequence();
    ssr_09_new_session_resets_sequence();
    ssr_10_reader_failures();
    ssr_11_cursor_race();
    ssr_12_two_sessions_compete();
    ssr_13_registration_changed();
    ssr_14_event_context_binding();
    ssr_15_delayed_old_session();
    ssr_16_missing_sequence();
    ssr_17_bad_repository_results();
    ssr_18_no_session_identity_or_sequence();

    if (failures != 0) {
        std::cerr << failures << " of " << checks << " checks failed.\n";
        return 1;
    }
    std::cout << "All " << checks << " SourceSession contract checks passed.\n";
    return 0;
}
