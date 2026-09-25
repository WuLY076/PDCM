#ifndef PDCM_IPC_HANDSHAKE_HPP_
#define PDCM_IPC_HANDSHAKE_HPP_

#include <optional>

#include "core/service_core.hpp"
#include "core/session_manager.hpp"
#include "ipc/frame.hpp"

namespace pdcm::ipc {

struct HandshakeResult {
  Status status;
  Frame response;
  std::optional<SessionId> session_id;
  bool close_connection{false};
};

class HandshakeHandler {
public:
  explicit HandshakeHandler(SessionManager *sessions);

  HandshakeResult handle(const Frame &request, const CoreSnapshot &core,
                         const ResourceLimits &limits,
                         const PeerIdentity &peer);

private:
  static Frame errorFrame(std::uint64_t request_id, pdcm_status_t status,
                          const char *stable_reason);

  SessionManager *sessions_;
};

} // namespace pdcm::ipc

#endif // PDCM_IPC_HANDSHAKE_HPP_
