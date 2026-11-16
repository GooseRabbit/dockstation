#include "source_session_verifier.hpp"

namespace dockstation::core {
namespace {

bool has_non_whitespace(std::string_view value) noexcept {
    for (const char ch : value) {
        if (ch != ' ' && ch != '\t' && ch != '\n' &&
            ch != '\r' && ch != '\f' && ch != '\v') {
            return true;
        }
    }
    return false;
}

bool is_empty_record(const SourceRegistrationRecord& record) noexcept {
    return record.station_id.empty() &&
           record.source.empty() &&
           record.registration_revision.empty() &&
           record.state == SourceRegistrationState::Unknown;
}

bool is_known_state(SourceRegistrationState state) noexcept {
    return state == SourceRegistrationState::Active ||
           state == SourceRegistrationState::Disabled ||
           state == SourceRegistrationState::Revoked;
}

SourceSessionVerificationResult failure(
    SourceSessionVerificationStatus status) noexcept {
    return {status, {}};
}

// TLS exporter output is fixed at 32 bytes by DEC-066 and the
// ControllerLink session-proof contract. It is channel binding, not identity.
constexpr std::size_t kSessionProofSize = 32U;

}  // namespace

bool is_valid_source_registration_lookup_result(
    const SourceRegistrationLookupResult& result) noexcept {
    switch (result.status) {
        case SourceRegistrationLookupStatus::Found:
            return has_non_whitespace(result.record.station_id) &&
                   has_non_whitespace(result.record.source) &&
                   has_non_whitespace(result.record.registration_revision) &&
                   is_known_state(result.record.state);
        case SourceRegistrationLookupStatus::NotFound:
        case SourceRegistrationLookupStatus::Unavailable:
        case SourceRegistrationLookupStatus::Error:
            return is_empty_record(result.record);
    }
    return false;
}

RegistrySourceSessionVerifier::RegistrySourceSessionVerifier(
    SourceRegistrationReader& registry,
    std::string_view local_station_id) noexcept
    : registry_(registry),
      local_station_id_(local_station_id),
      configured_(has_non_whitespace(local_station_id)) {}

SourceSessionVerificationResult
RegistrySourceSessionVerifier::verify_source_session(
    const SourceSessionEvidence& evidence) noexcept {
    // This policy checks the proof's shape only. It cannot verify TLS exporter
    // bytes or freshness itself; only the trusted transport adapter may create
    // SourceSessionEvidence from the currently live authenticated TLS channel.
    if (!configured_ || !is_valid_source_session_evidence(evidence) ||
        evidence.session_proof().size() != kSessionProofSize) {
        return failure(SourceSessionVerificationStatus::Error);
    }

    // The registry lookup uses the opaque pair identity. It never treats a
    // peer-supplied controller ID or Station ID as an identity credential.
    const SourceRegistrationLookupResult lookup =
        registry_.find_by_peer_identity(evidence.peer_identity_evidence());
    if (!is_valid_source_registration_lookup_result(lookup)) {
        return failure(SourceSessionVerificationStatus::Error);
    }

    switch (lookup.status) {
        case SourceRegistrationLookupStatus::NotFound:
            return failure(SourceSessionVerificationStatus::UnknownSource);
        case SourceRegistrationLookupStatus::Unavailable:
            return failure(SourceSessionVerificationStatus::Unavailable);
        case SourceRegistrationLookupStatus::Error:
            return failure(SourceSessionVerificationStatus::Error);
        case SourceRegistrationLookupStatus::Found:
            break;
    }

    const SourceRegistrationRecord& registration = lookup.record;
    if (registration.station_id != local_station_id_ ||
        registration.source != evidence.source()) {
        return failure(SourceSessionVerificationStatus::Rejected);
    }

    switch (registration.state) {
        case SourceRegistrationState::Active:
            return {
                SourceSessionVerificationStatus::Verified,
                {registration.source, evidence.source_instance(),
                 registration.registration_revision}
            };
        case SourceRegistrationState::Disabled:
        case SourceRegistrationState::Revoked:
            return failure(SourceSessionVerificationStatus::Rejected);
        case SourceRegistrationState::Unknown:
            return failure(SourceSessionVerificationStatus::Error);
    }

    return failure(SourceSessionVerificationStatus::Error);
}

}  // namespace dockstation::core
