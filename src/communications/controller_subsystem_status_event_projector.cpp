#include "communications/controller_subsystem_status_event_projector.hpp"

#include <algorithm>
#include <cstdint>
#include <variant>

#include "hardware/interfaces/controller_codec.hpp"
#include "hardware/interfaces/controller_protocol.hpp"

namespace dockstation::communications {
namespace {

using hardware::interfaces::ControllerErrorPayload;
using hardware::interfaces::ControllerErrorSeverity;
using hardware::interfaces::ControllerSubsystemStatus;

[[nodiscard]] std::string_view severity_name(
    ControllerErrorSeverity severity) noexcept {
    switch (severity) {
        case ControllerErrorSeverity::Info: return "INFO";
        case ControllerErrorSeverity::Warning: return "WARNING";
        case ControllerErrorSeverity::Error: return "ERROR";
        case ControllerErrorSeverity::Critical: return "CRITICAL";
        case ControllerErrorSeverity::Unknown: return {};
    }
    return {};
}

[[nodiscard]] std::string_view subsystem_status_name(
    ControllerSubsystemStatus status) noexcept {
    switch (status) {
        case ControllerSubsystemStatus::Pass: return "PASS";
        case ControllerSubsystemStatus::Degraded: return "DEGRADED";
        case ControllerSubsystemStatus::Fail: return "FAIL";
        case ControllerSubsystemStatus::Unknown: return "UNKNOWN";
    }
    return {};
}

[[nodiscard]] bool has_non_whitespace(std::string_view value) noexcept {
    for (const char ch : value) {
        if (ch != ' ' && ch != '\t' && ch != '\n' &&
            ch != '\r' && ch != '\f' && ch != '\v') {
            return true;
        }
    }
    return false;
}

}  // namespace

void ControllerSubsystemStatusEventProjector::reset() noexcept {
    payload_.fill('\0');
    payload_size_ = 0U;
}

bool ControllerSubsystemStatusEventProjector::append(
    std::string_view value) noexcept {
    if (payload_size_ > payload_.size() ||
        value.size() > payload_.size() - payload_size_) {
        return false;
    }
    std::copy(value.begin(), value.end(),
              payload_.begin() + static_cast<std::ptrdiff_t>(payload_size_));
    payload_size_ += value.size();
    return true;
}

bool ControllerSubsystemStatusEventProjector::append_json_string(
    std::string_view value) noexcept {
    constexpr char hex[] = "0123456789abcdef";
    if (!append("\"")) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (ch == static_cast<unsigned char>('"') ||
            ch == static_cast<unsigned char>('\\')) {
            const char escaped[2] = {'\\', static_cast<char>(ch)};
            if (!append({escaped, sizeof(escaped)})) {
                return false;
            }
        } else if (ch < 0x20U) {
            const char escaped[6] = {
                '\\', 'u', '0', '0',
                hex[(ch >> 4U) & 0x0FU], hex[ch & 0x0FU]
            };
            if (!append({escaped, sizeof(escaped)})) {
                return false;
            }
        } else {
            const char character = static_cast<char>(ch);
            if (!append({&character, 1U})) {
                return false;
            }
        }
    }
    return append("\"");
}

ControllerEventProjectionStatus
ControllerSubsystemStatusEventProjector::project(
    const hardware::interfaces::ControllerCodecDecodedMessage& message,
    ControllerEventProjection& output) noexcept {
    reset();
    output = {};

    if (message.envelope.type !=
        hardware::interfaces::ControllerMessageType::Error) {
        return ControllerEventProjectionStatus::NotApplicable;
    }

    if (hardware::interfaces::validate_controller_message_payload(
            message.envelope.type, message.payload).status !=
        hardware::interfaces::ControllerPayloadValidationStatus::Valid) {
        return ControllerEventProjectionStatus::Error;
    }

    const auto* error = std::get_if<ControllerErrorPayload>(&message.payload);
    if (error == nullptr) {
        return ControllerEventProjectionStatus::Error;
    }

    // Generic controller errors do not claim a subsystem state. In particular,
    // severity is never used to infer PASS/DEGRADED/FAIL.
    if (!error->subsystem_id.has_value() ||
        !error->subsystem_status.has_value()) {
        return ControllerEventProjectionStatus::NotApplicable;
    }

    const auto severity = severity_name(error->severity);
    const auto status = subsystem_status_name(*error->subsystem_status);
    if (!has_non_whitespace(*error->subsystem_id) ||
        !hardware::interfaces::is_valid_controller_wire_text(
            *error->subsystem_id) ||
        severity.empty() || status.empty()) {
        return ControllerEventProjectionStatus::Error;
    }

    const bool has_reason = !error->reason_text.empty();
    if (!append("{\"subsystem_id\":") ||
        !append_json_string(*error->subsystem_id) ||
        !append(",\"status\":") ||
        !append_json_string(status) ||
        !append(",\"error_code\":") ||
        !append_json_string(error->error_code) ||
        !append(",\"severity\":") ||
        !append_json_string(severity) ||
        !append(",\"description\":") ||
        !append_json_string(error->description)) {
        reset();
        return ControllerEventProjectionStatus::Error;
    }

    if (has_reason &&
        (!append(",\"reason_text\":") ||
         !append_json_string(error->reason_text))) {
        reset();
        return ControllerEventProjectionStatus::Error;
    }

    if (!append("}")) {
        reset();
        return ControllerEventProjectionStatus::Error;
    }

    output.event_type = "SUBSYSTEM_STATUS_UPDATED";
    output.payload = {payload_.data(), payload_size_};
    // VALID describes the authenticated, structurally valid report; the
    // reported subsystem state may still legitimately be UNKNOWN.
    output.quality_status = "VALID";
    output.schema_version = "1";
    if (has_reason) {
        output.reason_code = error->error_code;
        output.reason_text = error->reason_text;
    }
    return ControllerEventProjectionStatus::Projected;
}

}  // namespace dockstation::communications
