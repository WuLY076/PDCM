#ifndef PDCM_CLIENT_BACKEND_HPP_
#define PDCM_CLIENT_BACKEND_HPP_

#include <cstdint>
#include <string>

#include "common/status.hpp"
#include "core/service_core.hpp"

namespace pdcm {

enum class BackendMode : std::uint8_t {
  kStandalone,
  kEmbedded,
};

struct BackendVersion {
  std::string daemon_version;
  BackendMode mode{BackendMode::kStandalone};
  TargetKind target{TargetKind::kUnknown};
  CoreSnapshot core;
  ProviderFailurePhase provider_failure_phase{ProviderFailurePhase::kNone};
  bool provider_load_attempted{false};
  bool provider_loaded{false};
  bool provider_abi_compatible{false};
  std::uint16_t protocol_major{0};
  std::uint16_t protocol_minor{0};
  std::uint64_t session_id{0};
};

class ClientBackend {
public:
  virtual ~ClientBackend() = default;

  ClientBackend(const ClientBackend &) = delete;
  ClientBackend &operator=(const ClientBackend &) = delete;

  virtual Status start() = 0;
  virtual Status version(BackendVersion *version) const = 0;
  virtual Status close() noexcept = 0;

protected:
  ClientBackend() = default;
};

} // namespace pdcm

#endif // PDCM_CLIENT_BACKEND_HPP_
