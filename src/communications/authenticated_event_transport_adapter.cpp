#include "communications/authenticated_event_transport_adapter.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>

#include <openssl/crypto.h>

#include "hardware/interfaces/controller_codec.hpp"
#include "hardware/interfaces/controller_protocol.hpp"
#include "platform/linux/controller_psk_tls_client.hpp"

namespace dockstation::communications {
namespace {

using hardware::interfaces::ControllerMessageType;
using platform::linux_platform::ControllerLinkService;

constexpr std::size_t kReceiveTimestampCapacity = 32U;

struct SessionProofBuffer final {
    std::array<std::uint8_t,
               platform::linux_platform::kControllerSessionProofSize> bytes{};

    ~SessionProofBuffer() {
        OPENSSL_cleanse(bytes.data(), bytes.size());
    }
};

[[nodiscard]] bool is_supported_inbound_event_message(
    ControllerMessageType type) noexcept {
    return type == ControllerMessageType::TelemetrySnapshot ||
           type == ControllerMessageType::Error;
}

[[nodiscard]] bool append_text(
    std::array<char, AuthenticatedEventTransportAdapter::kEventIdCapacity>&
        destination,
    std::size_t& destination_size,
    std::string_view value) noexcept {
    if (destination_size > destination.size() ||
        value.size() > destination.size() - destination_size) {
        return false;
    }
    std::copy(value.begin(), value.end(),
              destination.begin() +
                  static_cast<std::ptrdiff_t>(destination_size));
    destination_size += value.size();
    return true;
}

[[nodiscard]] bool append_number(
    std::array<char, AuthenticatedEventTransportAdapter::kEventIdCapacity>&
        destination,
    std::size_t& destination_size,
    std::uint64_t value) noexcept {
    std::array<char, 32U> temporary{};
    const auto converted =
        std::to_chars(temporary.data(),
                      temporary.data() + temporary.size(), value, 10);
    if (converted.ec != std::errc{}) {
        return false;
    }
    return append_text(destination, destination_size,
        {temporary.data(),
         static_cast<std::size_t>(converted.ptr - temporary.data())});
}

// Length-prefixing makes the ID unambiguous even when source/instance contain
// punctuation used as separators. The fixed-size pair identity adds a station-
// independent namespace, so equal source/instance/sequence tuples from two
// separate PSK pairs cannot collide in a future shared event store.
[[nodiscard]] bool append_identity_hex(
    std::array<char, AuthenticatedEventTransportAdapter::kEventIdCapacity>&
        destination,
    std::size_t& destination_size,
    std::string_view identity) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char byte : identity) {
        const char pair[2] = {
            digits[(byte >> 4U) & 0x0FU],
            digits[byte & 0x0FU]
        };
        if (!append_text(destination, destination_size,
                         std::string_view{pair, sizeof(pair)})) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool make_event_id(
    std::string_view pair_identity,
    std::string_view source,
    std::string_view source_instance,
    std::uint64_t sequence,
    std::array<char, AuthenticatedEventTransportAdapter::kEventIdCapacity>&
        storage,
    std::size_t& storage_size) noexcept {
    storage.fill('\0');
    storage_size = 0U;
    return append_text(storage, storage_size, "controller-event:v1:") &&
           append_identity_hex(storage, storage_size, pair_identity) &&
           append_text(storage, storage_size, ":") &&
           append_number(storage, storage_size,
                         static_cast<std::uint64_t>(source.size())) &&
           append_text(storage, storage_size, ":") &&
           append_text(storage, storage_size, source) &&
           append_text(storage, storage_size, ":") &&
           append_number(storage, storage_size,
                         static_cast<std::uint64_t>(source_instance.size())) &&
           append_text(storage, storage_size, ":") &&
           append_text(storage, storage_size, source_instance) &&
           append_text(storage, storage_size, ":") &&
           append_number(storage, storage_size, sequence);
}

[[nodiscard]] bool format_received_at(
    std::uint64_t received_at_unix_ms,
    std::array<char, kReceiveTimestampCapacity>& storage,
    std::size_t& storage_size) noexcept {
    storage.fill('\0');
    const auto converted = std::to_chars(
        storage.data(), storage.data() + storage.size(),
        received_at_unix_ms, 10);
    if (converted.ec != std::errc{}) {
        storage_size = 0U;
        return false;
    }
    storage_size =
        static_cast<std::size_t>(converted.ptr - storage.data());
    return storage_size != 0U;
}

[[nodiscard]] bool successful_session(
    core::SourceSessionCoordinatorStatus status) noexcept {
    return status == core::SourceSessionCoordinatorStatus::Confirmed ||
           status == core::SourceSessionCoordinatorStatus::AlreadyCurrent;
}

}  // namespace

AuthenticatedEventTransportAdapter::AuthenticatedEventTransportAdapter(
    ControllerLinkService& service,
    core::StationEventPipeline& pipeline,
    ControllerEventProjector& projector,
    std::string_view configured_source,
    core::EventValidationPolicy validation_policy) noexcept
    : service_(service),
      pipeline_(pipeline),
      projector_(projector),
      validation_policy_(validation_policy) {
    // source is a local registration/configuration binding. It is never read
    // from the peer's payload or accepted as a peer-supplied controller ID.
    if (!copy_source(configured_source) || !pipeline_.is_configured()) {
        statistics_.last_status =
            AuthenticatedEventTransportStatus::InvalidConfiguration;
        return;
    }

    // The session-gated pipeline always requires the source's instance and
    // monotonically increasing sequence, regardless of caller defaults.
    validation_policy_.require_source_instance = true;
    validation_policy_.require_source_sequence = true;

    attached_ = service_.attach_message_handler(*this);
    statistics_.last_status = attached_
        ? AuthenticatedEventTransportStatus::Ready
        : AuthenticatedEventTransportStatus::InvalidConfiguration;
}

AuthenticatedEventTransportAdapter::~AuthenticatedEventTransportAdapter() {
    if (attached_) {
        service_.detach_message_handler(*this);
    }
}

void AuthenticatedEventTransportAdapter::on_authenticated_controller_message(
    ControllerLinkService& service,
    const ControllerCodecDecodedMessage& message,
    std::uint64_t received_at_unix_ms) noexcept {
    if (!attached_ || &service != &service_) {
        statistics_.last_status =
            AuthenticatedEventTransportStatus::ChannelUnavailable;
        ++statistics_.rejected_messages;
        return;
    }

    ++statistics_.authenticated_messages_received;

    if (!is_supported_inbound_event_message(message.envelope.type)) {
        statistics_.last_status =
            AuthenticatedEventTransportStatus::MessageIgnored;
        return;
    }

    const auto channel_status = service.status();
    const std::string_view pair_identity =
        service.authenticated_pair_identity();
    const std::string_view peer_controller_id =
        service.authenticated_peer_controller_id();

    if (channel_status.state !=
            platform::linux_platform::ControllerLinkServiceState::Established ||
        !channel_status.tls_authenticated ||
        !channel_status.protocol_established ||
        pair_identity.size() !=
            platform::linux_platform::kControllerPskIdentitySize ||
        peer_controller_id.empty()) {
        statistics_.last_status =
            AuthenticatedEventTransportStatus::ChannelUnavailable;
        ++statistics_.rejected_messages;
        return;
    }

    // ControllerLinkSession has already applied the exact peer ID, instance,
    // protocol version, and strictly-increasing sequence checks. Recheck the
    // identity binding at this boundary before any Event is projected.
    if (message.envelope.controller_id != peer_controller_id ||
        message.envelope.controller_instance_id.empty() ||
        !message.envelope.sequence_number.has_value() ||
        *message.envelope.sequence_number == 0U ||
        hardware::interfaces::validate_controller_message(
            message.envelope).status !=
            hardware::interfaces::ControllerMessageValidationStatus::Valid ||
        hardware::interfaces::validate_controller_message_payload(
            message.envelope.type, message.payload).status !=
            hardware::interfaces::ControllerPayloadValidationStatus::Valid) {
        statistics_.last_status =
            AuthenticatedEventTransportStatus::MessageBindingInvalid;
        ++statistics_.rejected_messages;
        return;
    }

    ControllerEventProjection projection{};
    const ControllerEventProjectionStatus projection_status =
        projector_.project(message, projection);
    if (projection_status ==
        ControllerEventProjectionStatus::NotApplicable) {
        statistics_.last_status =
            AuthenticatedEventTransportStatus::ProjectionNotApplicable;
        ++statistics_.unmapped_messages;
        return;
    }
    if (projection_status != ControllerEventProjectionStatus::Projected) {
        statistics_.last_status =
            AuthenticatedEventTransportStatus::ProjectionError;
        ++statistics_.rejected_messages;
        return;
    }

    std::array<char, kEventIdCapacity> event_id_storage{};
    std::size_t event_id_size = 0U;
    if (!make_event_id(
            pair_identity,
            {source_.data(), source_size_},
            message.envelope.controller_instance_id,
            *message.envelope.sequence_number,
            event_id_storage, event_id_size)) {
        statistics_.last_status =
            AuthenticatedEventTransportStatus::EventIdentifierUnavailable;
        ++statistics_.rejected_messages;
        return;
    }

    std::array<char, kReceiveTimestampCapacity> received_at_storage{};
    std::size_t received_at_size = 0U;
    if (!format_received_at(received_at_unix_ms,
                            received_at_storage, received_at_size)) {
        statistics_.last_status =
            AuthenticatedEventTransportStatus::ReceiveTimestampUnavailable;
        ++statistics_.rejected_messages;
        return;
    }

    SessionProofBuffer proof{};
    if (!service.export_authenticated_session_proof(proof.bytes)) {
        statistics_.last_status =
            AuthenticatedEventTransportStatus::ChannelUnavailable;
        ++statistics_.rejected_messages;
        return;
    }

    core::Event event{};
    event.event_id = {event_id_storage.data(), event_id_size};
    event.event_type = projection.event_type;
    event.source = {source_.data(), source_size_};
    event.source_instance = message.envelope.controller_instance_id;
    event.occurred_at = projection.occurred_at;
    event.received_at = {received_at_storage.data(), received_at_size};
    event.source_sequence = message.envelope.sequence_number;
    event.operation_id = projection.operation_id;
    event.correlation_id = projection.correlation_id;
    event.payload = projection.payload;
    event.quality_status = projection.quality_status;
    event.schema_version = projection.schema_version;
    event.reason_code = projection.reason_code;
    event.reason_text = projection.reason_text;

    const std::string_view proof_view{
        reinterpret_cast<const char*>(proof.bytes.data()),
        proof.bytes.size()};
    const core::SourceSessionEvidence evidence{
        event.source,
        *event.source_instance,
        pair_identity,
        proof_view
    };

    ++statistics_.projected_events;
    ++statistics_.pipeline_invocations;
    const core::StationEventPipelineResult result =
        pipeline_.handle(event, validation_policy_, evidence);
    statistics_.last_session_status = result.session_status;
    statistics_.last_event_status.reset();

    if (result.event_result.has_value()) {
        statistics_.last_event_status = result.event_result->status;
    }

    if (result.session_status.has_value() &&
        !successful_session(*result.session_status)) {
        statistics_.last_status =
            AuthenticatedEventTransportStatus::SessionRejected;
        return;
    }

    statistics_.last_status =
        AuthenticatedEventTransportStatus::PipelineInvoked;
}

bool AuthenticatedEventTransportAdapter::copy_source(
    std::string_view source) noexcept {
    if (source.empty() || source.size() > source_.size() ||
        !hardware::interfaces::is_valid_controller_wire_text(source)) {
        return false;
    }
    bool non_whitespace = false;
    for (const char ch : source) {
        if (ch != ' ' && ch != '\t' && ch != '\n' &&
            ch != '\r' && ch != '\f' && ch != '\v') {
            non_whitespace = true;
            break;
        }
    }
    if (!non_whitespace) {
        return false;
    }

    std::copy(source.begin(), source.end(), source_.begin());
    source_size_ = source.size();
    return true;
}

}  // namespace dockstation::communications
