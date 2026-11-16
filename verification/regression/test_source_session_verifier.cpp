#include "core/source_session_verifier.hpp"
#include "source_session_test_support.hpp"

#include <iostream>
#include <string>
#include <string_view>

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

class Registry final : public SourceRegistrationReader {
public:
    SourceRegistrationLookupResult result{};
    std::string queried_identity{};
    int calls{0};

    SourceRegistrationLookupResult find_by_peer_identity(
        std::string_view identity) noexcept override {
        ++calls;
        queried_identity.assign(identity.data(), identity.size());
        return result;
    }
};

SourceSessionEvidence make_evidence(
    std::string_view source = "sensor/esp32",
    std::string_view instance = "boot-2026-11-16",
    std::string_view identity = "0123456789abcdef",
    std::string_view proof = std::string_view{"0123456789abcdef0123456789abcdef"}) {
    return SourceSessionEvidenceTestAccess::make(
        source, instance, identity, proof);
}

SourceRegistrationLookupResult active_registration() {
    return {
        SourceRegistrationLookupStatus::Found,
        {"station-001", "sensor/esp32", "registration-rev-7",
         SourceRegistrationState::Active}
    };
}

void active_exact_registration_is_verified() {
    Registry registry;
    registry.result = active_registration();
    RegistrySourceSessionVerifier verifier{registry, "station-001"};
    const auto result = verifier.verify_source_session(make_evidence());

    expect(registry.calls == 1, "active record is looked up exactly once");
    expect(registry.queried_identity == "0123456789abcdef",
           "lookup uses the opaque pair identity, not the source string");
    expect(result.status == SourceSessionVerificationStatus::Verified,
           "matching ACTIVE registration is verified");
    expect(result.session.source == "sensor/esp32" &&
           result.session.source_instance == "boot-2026-11-16" &&
           result.session.registration_revision == "registration-rev-7",
           "verified result is bound to source, current instance and revision");
}

void unknown_and_unavailable_are_distinct() {
    Registry registry;
    RegistrySourceSessionVerifier verifier{registry, "station-001"};

    registry.result = {SourceRegistrationLookupStatus::NotFound, {}};
    auto result = verifier.verify_source_session(make_evidence());
    expect(result.status == SourceSessionVerificationStatus::UnknownSource,
           "missing registry identity maps to UnknownSource");

    registry.result = {SourceRegistrationLookupStatus::Unavailable, {}};
    result = verifier.verify_source_session(make_evidence());
    expect(result.status == SourceSessionVerificationStatus::Unavailable,
           "registry outage remains Unavailable");

    registry.result = {SourceRegistrationLookupStatus::Error, {}};
    result = verifier.verify_source_session(make_evidence());
    expect(result.status == SourceSessionVerificationStatus::Error,
           "registry read error remains Error");
}

void disabled_and_revoked_records_are_rejected() {
    Registry registry;
    RegistrySourceSessionVerifier verifier{registry, "station-001"};

    registry.result = active_registration();
    registry.result.record.state = SourceRegistrationState::Disabled;
    auto result = verifier.verify_source_session(make_evidence());
    expect(result.status == SourceSessionVerificationStatus::Rejected,
           "DISABLED registration is rejected");

    registry.result.record.state = SourceRegistrationState::Revoked;
    result = verifier.verify_source_session(make_evidence());
    expect(result.status == SourceSessionVerificationStatus::Rejected,
           "REVOKED registration is rejected");
}

void station_and_source_mismatch_are_rejected() {
    Registry registry;
    registry.result = active_registration();
    RegistrySourceSessionVerifier verifier{registry, "station-002"};
    auto result = verifier.verify_source_session(make_evidence());
    expect(result.status == SourceSessionVerificationStatus::Rejected,
           "registration for another Station ID is rejected");

    registry.result = active_registration();
    RegistrySourceSessionVerifier local_verifier{registry, "station-001"};
    result = local_verifier.verify_source_session(
        make_evidence("sensor/other"));
    expect(result.status == SourceSessionVerificationStatus::Rejected,
           "authenticated peer ID must match registered logical source");
}

void malformed_registry_answers_fail_closed() {
    Registry registry;
    RegistrySourceSessionVerifier verifier{registry, "station-001"};

    registry.result = {
        SourceRegistrationLookupStatus::Found,
        {"station-001", "sensor/esp32", "", SourceRegistrationState::Active}
    };
    auto result = verifier.verify_source_session(make_evidence());
    expect(result.status == SourceSessionVerificationStatus::Error,
           "incomplete FOUND record is a registry contract error");

    registry.result = {
        static_cast<SourceRegistrationLookupStatus>(255U), {}
    };
    result = verifier.verify_source_session(make_evidence());
    expect(result.status == SourceSessionVerificationStatus::Error,
           "unknown registry status fails closed");

    registry.result = active_registration();
    registry.result.record.state = static_cast<SourceRegistrationState>(255U);
    result = verifier.verify_source_session(make_evidence());
    expect(result.status == SourceSessionVerificationStatus::Error,
           "unknown registration state fails closed");

    registry.result = {
        SourceRegistrationLookupStatus::NotFound,
        {"station-001", "sensor/esp32", "unexpected", SourceRegistrationState::Active}
    };
    result = verifier.verify_source_session(make_evidence());
    expect(result.status == SourceSessionVerificationStatus::Error,
           "NOT_FOUND with a payload is a registry contract error");
}

void invalid_proof_and_invalid_station_configuration_do_not_query_registry() {
    Registry registry;
    registry.result = active_registration();
    RegistrySourceSessionVerifier verifier{registry, "station-001"};

    auto result = verifier.verify_source_session(
        make_evidence("sensor/esp32", "boot-2026-11-16",
                      "0123456789abcdef", "too-short"));
    expect(result.status == SourceSessionVerificationStatus::Error &&
           registry.calls == 0,
           "session proof must be exactly 32 bytes before registry access");

    RegistrySourceSessionVerifier invalid_verifier{registry, "  \t"};
    result = invalid_verifier.verify_source_session(make_evidence());
    expect(result.status == SourceSessionVerificationStatus::Error &&
           registry.calls == 0,
           "missing local Station ID fails closed before registry access");
}

void binary_identity_is_compared_as_opaque_bytes() {
    std::string identity{"A\0B\0C", 5U};
    Registry registry;
    registry.result = active_registration();
    RegistrySourceSessionVerifier verifier{registry, "station-001"};
    const auto result = verifier.verify_source_session(
        make_evidence("sensor/esp32", "boot-1", identity));

    expect(result.status == SourceSessionVerificationStatus::Verified,
           "binary PSK identity containing NUL bytes is accepted as opaque evidence");
    expect(registry.queried_identity == identity,
           "registry lookup preserves every identity byte");
}
}

int main() {
    active_exact_registration_is_verified();
    unknown_and_unavailable_are_distinct();
    disabled_and_revoked_records_are_rejected();
    station_and_source_mismatch_are_rejected();
    malformed_registry_answers_fail_closed();
    invalid_proof_and_invalid_station_configuration_do_not_query_registry();
    binary_identity_is_compared_as_opaque_bytes();

    if (failures != 0) {
        std::cerr << failures << " of " << checks << " checks failed.\n";
        return 1;
    }
    std::cout << "All " << checks
              << " registry-backed source-session verifier checks passed.\n";
}
