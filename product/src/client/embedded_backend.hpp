#ifndef PDCM_CLIENT_EMBEDDED_BACKEND_HPP_
#define PDCM_CLIENT_EMBEDDED_BACKEND_HPP_

#include <memory>
#include <mutex>

#include "client/backend.hpp"
#include "common/clock.hpp"
#include "core/runtime_config.hpp"
#include "core/session_manager.hpp"
#include "provider/provider.hpp"

namespace pdcm {

class EmbeddedBackend final : public ClientBackend {
public:
  EmbeddedBackend(RuntimeConfig config, std::unique_ptr<Provider> provider,
                  std::shared_ptr<const Clock> clock);
  ~EmbeddedBackend() override;

  Status start() override;
  Status version(BackendVersion *version) const override;
  Status close() noexcept override;

private:
  RuntimeConfig config_;
  PdcmServiceCore core_;
  SessionManager sessions_;

  mutable std::mutex mutex_;
  SessionId session_id_{0};
  bool started_{false};
  bool closed_{false};
};

} // namespace pdcm

#endif // PDCM_CLIENT_EMBEDDED_BACKEND_HPP_
