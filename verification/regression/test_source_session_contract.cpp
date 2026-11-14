#include "core/event_processing.hpp"
#include "core/source_session.hpp"
#include "core/source_session_coordinator.hpp"
#include "source_session_test_support.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace dockstation::core;
using dockstation::verification::SourceSessionEvidenceTestAccess;

int failures = 0;
int checks = 0;

void expect(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

SourceSessionEvidence evidence(
    std::string_view source = "esp32/main",
    std::string_view source_instance = "boot-new",
    std::string_view peer_identity_evidence = "trusted-peer",
    std::string_view session_proof = "fresh-proof") {
    return SourceSessionEvidenceTestAccess::make(
        source, source_instance, peer_identity_evidence, session_proof);
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
        if (input.peer_identity_evidence() != "trusted-peer" ||
            input.session_proof() != "fresh-proof") {
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

Event event_for_evidence(const SourceSessionEvidence& input) {
    Event event{};
    event.source = input.source()
    event.source_instance = input.source_instance()
    return event;
}

SourceSessionCoordinatorResult run_coordinator(
    const Event& event, const SourceSessionEvidence& input,
    SourceSessionVerifier& verifier, SourceCursorReader& reader,
    SourceSessionRepository& repository) noexcept {
    SourceSessionCoordinator coordinator{verifier, reader, repository};
    return coordinator.confirm(event, input);
}

Flow run_flow(const Event& event, const SourceSessionEvidence& input,
              SourceSessionVerifier& verifier, SourceCursorReader& reader,
              SourceSessionRepository& repository) noexcept {
    const auto result = run_coordinator(event, input, verifier, reader, repository);
    switch (result.status) {
        case SourceSessionCoordinatorStatus::Confirmed: return Flow::Confirmed;
        case SourceSessionCoordinatorStatus::AlreadyCurrent: return Flow::AlreadyCurrent;
        case SourceSessionCoordinatorStatus::InvalidEvidence:
        case SourceSessionCoordinatorStatus::Rejected: return Flow::Rejected;
        case SourceSessionCoordinatorStatus::UnknownSource: return Flow::Unknown;
        case SourceSessionCoordinatorStatus::Unavailable: return Flow::Unavailable;
        case SourceSessionCoordinatorStatus::VerificationError: return Flow::VerifyError;
        case SourceSessionCoordinatorStatus::CursorReadError: return Flow::ReadError;
        case SourceSessionCoordinatorStatus::CursorChanged: return Flow::CursorChanged;
        case SourceSessionCoordinatorStatus::RegistrationChanged:
            return Flow::RegistrationChanged;
        case SourceSessionCoordinatorStatus::RepositoryError: return Flow::CommitError;
    }
    return Flow::CommitError;
}

Flow run_flow(const SourceSessionEvidence& input, SourceSessionVerifier& verifier,
              SourceCursorReader& reader,
              SourceSessionRepository& repository) noexcept {
    return run_flow(event_for_evidence(input), input, verifier, reader, repository);
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
    const auto e = evidence("esp32/main", "boot-new", "other-peer", "fresh-proof");
    expect(run_flow(e, v, r, p) == Flow::Rejected, "SSR-03 peer identity mismatch");
    expect(p.calls == 0, "SSR-03 no cursor mutation");
}
void ssr_04_replayed_proof() {
    FakeVerifier v; FakeReader r; FakeRepository p;
    const auto e = evidence("esp32/main", "boot-new", "trusted-peer", "replayed");
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
    const auto outcome = run_coordinator(event_for_evidence(evidence()), evidence(), v, r, p);
    expect(outcome.status == SourceSessionCoordinatorStatus::Confirmed,
           "SSR-06 first session confirmed by production coordinator");
    expect(outcome.cursor.source == "esp32/main" &&
           outcome.cursor.source_instance == std::optional<std::string_view>{"boot-new"} &&
           !outcome.cursor.last_sequence, "SSR-06 exact initial cursor returned without sequence");
    const auto stored = p.result();
    expect(stored.status == SourceCursorReadStatus::Found &&
           same_cursor(outcome.cursor, stored.cursor),
           "SSR-06 returned cursor matches repository state");
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
    const auto outcome = run_coordinator(event_for_evidence(evidence()), evidence(), v, r, p);
    expect(outcome.status == SourceSessionCoordinatorStatus::AlreadyCurrent,
           "SSR-08 same instance is AlreadyCurrent");
    expect(outcome.cursor.last_sequence == std::optional<std::uint64_t>{73U} &&
           p.result().cursor.last_sequence == std::optional<std::uint64_t>{73U},
           "SSR-08 last_sequence is preserved in returned and stored cursor");
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
void ssr_14_event_context_binding() {
    FakeVerifier matching_verifier; FakeReader matching_reader; FakeRepository matching_repo;
    const Event matching_event = event_for_evidence(evidence());
    expect(run_flow(matching_event, evidence(), matching_verifier,
                    matching_reader, matching_repo) == Flow::Confirmed,
           "SSR-14 matching event/evidence reaches session confirmation");

    FakeVerifier wrong_instance_verifier; FakeReader wrong_instance_reader;
    FakeRepository wrong_instance_repo;
    Event wrong_instance = event_for_evidence(evidence());
    wrong_instance.source_instance = "boot-old";
    expect(run_flow(wrong_instance, evidence(), wrong_instance_verifier,
                    wrong_instance_reader, wrong_instance_repo) == Flow::Rejected,
           "SSR-14 wrong instance is rejected by production coordinator");
    expect(wrong_instance_verifier.calls == 0 && wrong_instance_reader.calls == 0 &&
           wrong_instance_repo.calls == 0,
           "SSR-14 wrong instance is blocked before verifier/reader/repository");

    FakeVerifier wrong_source_verifier; FakeReader wrong_source_reader;
    FakeRepository wrong_source_repo;
    Event wrong_source = event_for_evidence(evidence());
    wrong_source.source = "esp32/other";
    expect(run_flow(wrong_source, evidence(), wrong_source_verifier,
                    wrong_source_reader, wrong_source_repo) == Flow::Rejected,
           "SSR-14 wrong source is rejected by production coordinator");
    expect(wrong_source_verifier.calls == 0 && wrong_source_reader.calls == 0 &&
           wrong_source_repo.calls == 0,
           "SSR-14 wrong source is blocked before all trust ports");
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
    const auto e = evidence("esp32/main", "", "trusted-peer", "fresh-proof");
    expect(!is_valid_source_session_evidence(e),
           "SSR-18 session verifier requires a non-empty source_instance");

    FakeVerifier verifier; FakeReader reader; FakeRepository repository;
    const Event invalid_event = event_for_evidence(e);
    const auto invalid = run_coordinator(invalid_event, e, verifier, reader, repository);
    expect(invalid.status == SourceSessionCoordinatorStatus::InvalidEvidence,
           "SSR-18 coordinator rejects missing session identity");
    expect(verifier.calls == 0 && reader.calls == 0 && repository.calls == 0,
           "SSR-18 missing session identity reaches no trust port");

    Event event{}; event.source = "legacy/sensor";
    const SourceCursor cursor{"legacy/sensor", std::nullopt, std::nullopt};
    const auto result = check_event_freshness(event, cursor);
    expect(result.status == EventFreshnessStatus::Unordered &&
           result.reason == EventFreshnessReason::SequenceMissing,
           "SSR-18 no sequence remains unordered");
}

void ssr_19_same_live_channel_can_confirm_multiple_events() {
    FakeVerifier v; FakeReader r; FakeRepository p;
    p.set("esp32/main", std::string{"boot-new"}, 73U);
    r.status = SourceCursorReadStatus::Found;
    r.instance = "boot-new";
    r.sequence = 73U;
    const auto first = run_coordinator(event_for_evidence(evidence()), evidence(), v, r, p);
    const auto second = run_coordinator(event_for_evidence(evidence()), evidence(), v, r, p);
    expect(first.status == SourceSessionCoordinatorStatus::AlreadyCurrent &&
           second.status == SourceSessionCoordinatorStatus::AlreadyCurrent,
           "SSR-19 repeated events on the same live authenticated channel remain valid");
    expect(v.calls == 2 && r.calls == 2 && p.calls == 2,
           "SSR-19 each event rechecks the live channel and current registration/cursor");
    expect(second.cursor.last_sequence == std::optional<std::uint64_t>{73U},
           "SSR-19 revalidation does not reset the current sequence");
}

void ssr_20_reject_blank_text_identifiers_and_error_cursor_success() {
    const auto blank_source = evidence("  \t ", "boot-new", "trusted-peer", "fresh-proof");
    expect(!is_valid_source_session_evidence(blank_source),
           "SSR-20 whitespace-only source is rejected");
    const auto blank_instance = evidence("esp32/main", " \n", "trusted-peer", "fresh-proof");
    expect(!is_valid_source_session_evidence(blank_instance),
           "SSR-20 whitespace-only source_instance is rejected");
    const VerifiedSourceSession blank_revision{"esp32/main", "boot-new", " \t"};
    expect(!is_valid_verified_source_session(blank_revision),
           "SSR-20 whitespace-only registration revision is rejected");

    const SourceCursorReadResult failed_read{SourceCursorReadStatus::Error, {}};
    const VerifiedSourceSession trusted{"esp32/main", "boot-new", "reg-7"};
    const SourceSessionConfirmationResult forged_success{
        SourceSessionConfirmationStatus::Confirmed,
        {"esp32/main", std::string_view{"boot-new"}, std::nullopt}};
    expect(!is_valid_source_session_confirmation_result(
               failed_read, trusted, forged_success),
           "SSR-20 Error cursor result cannot validate a successful confirmation");

    const SourceCursorReadResult blank_instance_cursor{
        SourceCursorReadStatus::Found,
        {"esp32/main", std::string_view{"  "}, 10U}};
    expect(!is_valid_source_cursor_read_result("esp32/main", blank_instance_cursor),
           "SSR-20 Found cursor rejects present-but-blank source_instance");
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
    ssr_19_same_live_channel_can_confirm_multiple_events();
    ssr_20_reject_blank_text_identifiers_and_error_cursor_success();

    if (failures != 0) {
        std::cerr << failures << " of " << checks << " checks failed.\n";
        return 1;
    }
    std::cout << "All " << checks << " SourceSession contract checks passed.\n";
    return 0;
}
