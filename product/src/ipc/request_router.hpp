#ifndef PDCM_IPC_REQUEST_ROUTER_HPP_
#define PDCM_IPC_REQUEST_ROUTER_HPP_

#include "core/service_core.hpp"
#include "ipc/frame.hpp"

namespace pdcm::ipc {

struct RequestRouteResult {
  Frame response;
  bool close_connection{false};
};

class RequestRouter {
public:
  explicit RequestRouter(PdcmServiceCore *core);

  [[nodiscard]] RequestRouteResult route(const Frame &request) const;

private:
  [[nodiscard]] static Frame errorFrame(std::uint64_t request_id,
                                        pdcm_status_t status,
                                        const char *stable_reason);

  PdcmServiceCore *core_;
};

} // namespace pdcm::ipc

#endif // PDCM_IPC_REQUEST_ROUTER_HPP_
