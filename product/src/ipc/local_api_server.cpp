#include "ipc/local_api_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace pdcm::ipc {
namespace {

Status serverSocketError(const int error, const char *const operation) {
  switch (error) {
  case EACCES:
  case EPERM:
    return Status(PDCM_STATUS_PERMISSION_DENIED, operation);
  case EADDRINUSE:
    return Status(PDCM_STATUS_RESOURCE_EXHAUSTED, operation);
  default:
    return Status(PDCM_STATUS_INTERNAL, operation);
  }
}

MonotonicTime writeDeadline() {
  return std::chrono::time_point_cast<Nanoseconds>(
             std::chrono::steady_clock::now()) +
         std::chrono::seconds(1);
}

} // namespace

LocalApiServer::LocalApiServer(std::string endpoint,
                               PdcmServiceCore *const core,
                               const ResourceLimits limits)
    : endpoint_(std::move(endpoint)), core_(core), limits_(limits),
      sessions_(limits), handshake_(&sessions_), router_(core) {}

LocalApiServer::~LocalApiServer() { (void)stop(); }

Status LocalApiServer::start() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (started_once_ || running_.load() || listen_socket_.valid()) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "local API server can only be started once");
  }
  if (core_ == nullptr || limits_.max_sessions == 0 ||
      limits_.max_frame_bytes < kFrameHeaderBytes) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "local API server configuration is invalid");
  }
  const CoreState core_state = core_->snapshot().state;
  if (core_state != CoreState::kReady && core_state != CoreState::kDegraded) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "core must be ready or degraded before binding IPC");
  }

  sockaddr_un address{};
  if (endpoint_.empty() || endpoint_.front() != '/' ||
      endpoint_.size() >= sizeof(address.sun_path)) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "Unix socket endpoint is invalid");
  }

  UniqueFd candidate(
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!candidate.valid()) {
    return serverSocketError(errno, "Unix socket creation failed");
  }

  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, endpoint_.c_str(), endpoint_.size() + 1);
  const std::size_t raw_length =
      offsetof(sockaddr_un, sun_path) + endpoint_.size() + 1;
  if (raw_length > std::numeric_limits<socklen_t>::max()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "Unix socket endpoint length is invalid");
  }

  if (::bind(candidate.get(), reinterpret_cast<sockaddr *>(&address),
             static_cast<socklen_t>(raw_length)) != 0) {
    return serverSocketError(errno, "Unix socket bind failed");
  }
  bound_ = true;

  if (::chmod(endpoint_.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) != 0) {
    const Status status =
        serverSocketError(errno, "Unix socket permission setup failed");
    (void)::unlink(endpoint_.c_str());
    bound_ = false;
    return status;
  }

  const std::size_t capped_backlog =
      std::min(limits_.max_sessions, static_cast<std::size_t>(INT_MAX));
  const int backlog = std::max(1, static_cast<int>(capped_backlog));
  if (::listen(candidate.get(), backlog) != 0) {
    const Status status = serverSocketError(errno, "Unix socket listen failed");
    (void)::unlink(endpoint_.c_str());
    bound_ = false;
    return status;
  }

  try {
    workers_.reserve(limits_.max_sessions);
  } catch (...) {
    (void)::unlink(endpoint_.c_str());
    bound_ = false;
    return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                  "local API worker capacity allocation failed");
  }

  listen_socket_ = std::move(candidate);
  running_.store(true);
  try {
    accept_thread_ = std::thread(&LocalApiServer::acceptLoop, this);
  } catch (...) {
    running_.store(false);
    listen_socket_.reset();
    (void)::unlink(endpoint_.c_str());
    bound_ = false;
    return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                  "local API accept thread creation failed");
  }
  started_once_ = true;
  return Status::success();
}

Status LocalApiServer::stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!listen_socket_.valid() && !bound_) {
      return Status::success();
    }
    running_.store(false);
    if (listen_socket_.valid()) {
      (void)::shutdown(listen_socket_.get(), SHUT_RDWR);
    }
  }

  {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    for (const int fd : connection_fds_) {
      (void)::shutdown(fd, SHUT_RDWR);
    }
  }

  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  reapWorkers(true);

  (void)sessions_.beginDraining();
  sessions_.closeAll();

  std::lock_guard<std::mutex> lock(state_mutex_);
  listen_socket_.reset();
  if (bound_) {
    (void)::unlink(endpoint_.c_str());
    bound_ = false;
  }
  return Status::success();
}

std::size_t LocalApiServer::activeSessions() const { return sessions_.size(); }

std::size_t LocalApiServer::completedConnections() const noexcept {
  return completed_connections_.load();
}

bool LocalApiServer::reserveConnection() noexcept {
  std::size_t current = active_connections_.load();
  while (current < limits_.max_sessions) {
    if (active_connections_.compare_exchange_weak(current, current + 1)) {
      return true;
    }
  }
  return false;
}

void LocalApiServer::acceptLoop() noexcept {
  while (running_.load()) {
    pollfd descriptor{};
    descriptor.fd = listen_socket_.get();
    descriptor.events = POLLIN;
    const int polled = ::poll(&descriptor, 1, 100);
    if (polled < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (polled == 0) {
      reapWorkers(false);
      continue;
    }
    if ((descriptor.revents & POLLIN) == 0) {
      if (!running_.load()) {
        break;
      }
      continue;
    }

    const int client = ::accept4(listen_socket_.get(), nullptr, nullptr,
                                 SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      if (!running_.load()) {
        break;
      }
      continue;
    }
    reapWorkers(false);
    if (workers_.size() >= limits_.max_sessions || !reserveConnection()) {
      (void)::close(client);
      continue;
    }

    try {
      auto done = std::make_shared<std::atomic<bool>>(false);
      std::thread worker([this, client, done] {
        handleConnection(client);
        active_connections_.fetch_sub(1);
        done->store(true);
      });
      workers_.push_back(Worker{std::move(worker), std::move(done)});
    } catch (...) {
      active_connections_.fetch_sub(1);
      (void)::close(client);
    }
    reapWorkers(false);
  }
  reapWorkers(false);
}

void LocalApiServer::handleConnection(const int fd) noexcept {
  UniqueFd connection(fd);
  std::optional<SessionId> session_id;
  bool registered = false;

  try {
    ucred credentials{};
    socklen_t credential_size = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials,
                     &credential_size) != 0 ||
        credential_size != sizeof(credentials) || credentials.pid <= 0) {
      completed_connections_.fetch_add(1);
      return;
    }

    {
      std::lock_guard<std::mutex> lock(connection_mutex_);
      connection_fds_.insert(fd);
      registered = true;
    }

    const PeerIdentity peer{credentials.uid, credentials.gid,
                            static_cast<std::uint64_t>(credentials.pid)};
    FrameDecoder decoder(limits_.max_frame_bytes);
    bool handshake_complete = false;
    bool close_connection = false;
    std::array<std::uint8_t, 4096> buffer{};
    const MonotonicTime handshake_deadline =
        std::chrono::time_point_cast<Nanoseconds>(
            std::chrono::steady_clock::now()) +
        std::chrono::seconds(5);

    while (running_.load() && !close_connection) {
      if (!handshake_complete &&
          std::chrono::steady_clock::now() >= handshake_deadline) {
        break;
      }
      pollfd descriptor{};
      descriptor.fd = fd;
      descriptor.events = POLLIN;
      const int polled = ::poll(&descriptor, 1, 100);
      if (polled < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (polled == 0) {
        continue;
      }
      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
          (descriptor.revents & POLLIN) == 0) {
        break;
      }

      const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (received < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        break;
      }
      if (received == 0) {
        break;
      }

      std::vector<Frame> frames;
      Status decoded = decoder.append(
          buffer.data(), static_cast<std::size_t>(received), &frames);
      if (!decoded.ok()) {
        break;
      }

      for (const Frame &frame : frames) {
        Frame response;
        if (handshake_complete) {
          RequestRouteResult result = router_.route(frame);
          response = std::move(result.response);
          close_connection = result.close_connection;
        } else {
          HandshakeResult result =
              handshake_.handle(frame, core_->snapshot(), limits_, peer);
          response = std::move(result.response);
          session_id = result.session_id;
          handshake_complete = result.status.ok() && session_id.has_value();
          close_connection = result.close_connection;
        }

        std::vector<std::uint8_t> response_bytes;
        Status encoded =
            encodeFrame(response, limits_.max_frame_bytes, &response_bytes);
        if (!encoded.ok() || !writeAll(fd, response_bytes.data(),
                                       response_bytes.size(), writeDeadline())
                                  .ok()) {
          close_connection = true;
          break;
        }
      }
    }
  } catch (...) {
    (void)::shutdown(fd, SHUT_RDWR);
  }

  if (session_id.has_value()) {
    (void)sessions_.close(*session_id);
  }
  if (registered) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    connection_fds_.erase(fd);
  }
  completed_connections_.fetch_add(1);
}

void LocalApiServer::reapWorkers(const bool join_all) noexcept {
  auto iterator = workers_.begin();
  while (iterator != workers_.end()) {
    if (join_all || iterator->done->load()) {
      if (iterator->thread.joinable()) {
        iterator->thread.join();
      }
      iterator = workers_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

} // namespace pdcm::ipc
