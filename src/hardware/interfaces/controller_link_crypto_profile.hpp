#pragma once
#include <cstddef>
namespace dockstation::hardware::interfaces {
inline constexpr std::size_t kControllerLinkPskSize = 32U;
inline constexpr std::size_t kControllerLinkPskIdentitySize = 16U;
inline constexpr std::size_t kControllerLinkSessionProofSize = 32U;
inline constexpr char kControllerLinkSessionProofExporterLabel[] = "EXPORTER-DockStation-ControllerLink-v1";
inline constexpr char kControllerLinkSessionProofContext[] = "DockStation/ControllerLink/session-proof/v1";
inline constexpr char kControllerLinkTlsCipherSuite[] = "TLS_AES_128_GCM_SHA256";
inline constexpr char kControllerLinkTlsNamedGroup[] = "P-256";
}  // namespace dockstation::hardware::interfaces
