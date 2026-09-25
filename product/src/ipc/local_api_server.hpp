#ifndef PDCM_IPC_LOCAL_API_SERVER_HPP_
#define PDCM_IPC_LOCAL_API_SERVER_HPP_

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/service_core.hpp"
#include "core/session_manager.hpp"
#include "ipc/handshake.hpp"
#include "ipc/request_router.hpp"
#include "ipc/unix_socket.hpp"

namespace pdcm::ipc {

class LocalApiServer {
public:
  LocalApiServer(std::string endpoint, PdcmServiceCore *core,
                 ResourceLimits limits);
  ~LocalApiServer();

  LocalApiServer(const LocalApiServer &) = delete;
  LocalApiServer &operator=(const LocalApiServer &) = delete;

  Status start();
  Status stop() noexcept;

  [[nodiscard]] std::size_t activeSessions() const;
  [[nodiscard]] std::size_t completedConnections() const noexcept;

private:
  struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };

  void acceptLoop() noexcept;
  void handleConnection(int fd) noexcept;
  void reapWorkers(bool join_all) noexcept;
  bool reserveConnection() noexcept;

  std::string endpoint_;
  PdcmServiceCore *core_;
  ResourceLimits limits_;
  SessionManager sessions_;
  HandshakeHandler handshake_;
  RequestRouter router_;

  mutable std::mutex state_mutex_;
  UniqueFd listen_socket_;
  bool bound_{false};
  bool started_once_{false};
  std::atomic<bool> running_{false};
  std::thread accept_thread_;

  std::atomic<std::size_t> active_connections_{0};
  std::atomic<std::size_t> completed_connections_{0};
  std::mutex connection_mutex_;
  std::set<int> connection_fds_;

  std::vector<Worker> workers_;
};

} // namespace pdcm::ipc

#endif // PDCM_IPC_LOCAL_API_SERVER_HPP_
