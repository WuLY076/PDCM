#include "ipc/unix_socket.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace pdcm::ipc {
namespace {

Status socketError(const int error, const char *const operation) {
  switch (error) {
  case EACCES:
  case EPERM:
    return Status(PDCM_STATUS_PERMISSION_DENIED, operation);
  case ETIMEDOUT:
    return Status(PDCM_STATUS_TIMEOUT, operation);
  case ENOENT:
  case ECONNREFUSED:
  case ECONNRESET:
  case ENOTCONN:
  case EPIPE:
    return Status(PDCM_STATUS_UNAVAILABLE, operation);
  default:
    return Status(PDCM_STATUS_INTERNAL, operation);
  }
}

int timeoutMilliseconds(const MonotonicTime deadline) {
  const MonotonicTime now = std::chrono::time_point_cast<Nanoseconds>(
      std::chrono::steady_clock::now());
  if (deadline <= now) {
    return 0;
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  if (remaining.count() >= INT_MAX) {
    return INT_MAX;
  }
  return std::max(1, static_cast<int>(remaining.count()));
}

enum class WaitDirection : std::uint8_t {
  kRead,
  kWrite,
};

Status waitFor(const int fd, const WaitDirection direction,
               const MonotonicTime deadline) {
  while (true) {
    const MonotonicTime now = std::chrono::time_point_cast<Nanoseconds>(
        std::chrono::steady_clock::now());
    if (deadline <= now) {
      return Status(PDCM_STATUS_TIMEOUT, "local IPC deadline expired");
    }

    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = direction == WaitDirection::kRead ? POLLIN : POLLOUT;
    const int result = ::poll(&descriptor, 1, timeoutMilliseconds(deadline));
    if (result == 0) {
      return Status(PDCM_STATUS_TIMEOUT, "local IPC deadline expired");
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return socketError(errno, "local IPC poll failed");
    }
    if ((descriptor.revents & POLLNVAL) != 0) {
      return Status(PDCM_STATUS_INTERNAL, "local IPC descriptor is invalid");
    }
    if ((descriptor.revents & (POLLERR | POLLHUP)) != 0 &&
        (descriptor.revents & descriptor.events) == 0) {
      return Status(PDCM_STATUS_UNAVAILABLE, "local IPC peer disconnected");
    }
    if ((descriptor.revents & descriptor.events) != 0) {
      return Status::success();
    }
  }
}

Status validateEndpoint(const std::string &endpoint) {
  sockaddr_un address{};
  if (endpoint.empty() || endpoint.front() != '/' ||
      endpoint.size() >= sizeof(address.sun_path)) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "Unix socket endpoint is invalid");
  }
  return Status::success();
}

} // namespace

UniqueFd::UniqueFd(const int fd) noexcept : fd_(fd) {}

UniqueFd::~UniqueFd() { reset(); }

UniqueFd::UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}

UniqueFd &UniqueFd::operator=(UniqueFd &&other) noexcept {
  if (this != &other) {
    reset(other.release());
  }
  return *this;
}

int UniqueFd::get() const noexcept { return fd_; }

bool UniqueFd::valid() const noexcept { return fd_ >= 0; }

int UniqueFd::release() noexcept {
  const int current = fd_;
  fd_ = -1;
  return current;
}

void UniqueFd::reset(const int fd) noexcept {
  if (fd_ >= 0) {
    (void)::close(fd_);
  }
  fd_ = fd;
}

Status connectUnixSocket(const std::string &endpoint,
                         const MonotonicTime deadline, UniqueFd *const socket) {
  if (socket == nullptr || socket->valid()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT, "socket output must be empty");
  }
  Status endpoint_status = validateEndpoint(endpoint);
  if (!endpoint_status.ok()) {
    return endpoint_status;
  }

  UniqueFd candidate(
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!candidate.valid()) {
    return socketError(errno, "Unix socket creation failed");
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
  const std::size_t raw_length =
      offsetof(sockaddr_un, sun_path) + endpoint.size() + 1;
  if (raw_length > std::numeric_limits<socklen_t>::max()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "Unix socket endpoint length is invalid");
  }

  const int connected =
      ::connect(candidate.get(), reinterpret_cast<sockaddr *>(&address),
                static_cast<socklen_t>(raw_length));
  if (connected != 0) {
    if (errno != EINPROGRESS) {
      return socketError(errno, "Unix socket connection failed");
    }
    Status writable = waitFor(candidate.get(), WaitDirection::kWrite, deadline);
    if (!writable.ok()) {
      return writable;
    }

    int connection_error = 0;
    socklen_t error_size = sizeof(connection_error);
    if (::getsockopt(candidate.get(), SOL_SOCKET, SO_ERROR, &connection_error,
                     &error_size) != 0) {
      return socketError(errno, "Unix socket status check failed");
    }
    if (connection_error != 0) {
      return socketError(connection_error, "Unix socket connection failed");
    }
  }

  *socket = std::move(candidate);
  return Status::success();
}

Status checkUnixSocketConnected(const int fd) {
  if (fd < 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT, "invalid local IPC descriptor");
  }

  pollfd descriptor{};
  descriptor.fd = fd;
  descriptor.events = POLLIN;
  const int result = ::poll(&descriptor, 1, 0);
  if (result < 0) {
    return socketError(errno, "local IPC status check failed");
  }
  if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    return Status(PDCM_STATUS_UNAVAILABLE, "local IPC peer disconnected");
  }
  if ((descriptor.revents & POLLIN) != 0) {
    std::uint8_t byte = 0;
    const ssize_t peeked = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (peeked == 0) {
      return Status(PDCM_STATUS_UNAVAILABLE,
                    "local IPC peer closed the connection");
    }
    if (peeked < 0 && errno != EINTR && errno != EAGAIN &&
        errno != EWOULDBLOCK) {
      return socketError(errno, "local IPC status check failed");
    }
  }
  return Status::success();
}

Status writeAll(const int fd, const std::uint8_t *const data,
                const std::size_t size, const MonotonicTime deadline) {
  if (fd < 0 || (data == nullptr && size != 0)) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "invalid local IPC write arguments");
  }

  std::size_t written = 0;
  while (written < size) {
    Status writable = waitFor(fd, WaitDirection::kWrite, deadline);
    if (!writable.ok()) {
      return writable;
    }

    const ssize_t result =
        ::send(fd, data + written, size - written, MSG_NOSIGNAL);
    if (result < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return socketError(errno, "local IPC write failed");
    }
    if (result == 0) {
      return Status(PDCM_STATUS_UNAVAILABLE,
                    "local IPC peer stopped accepting data");
    }
    written += static_cast<std::size_t>(result);
  }
  return Status::success();
}

Status readOneFrame(const int fd, Frame *const frame,
                    const std::size_t max_frame_bytes,
                    const MonotonicTime deadline) {
  if (fd < 0 || frame == nullptr) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "invalid local IPC read arguments");
  }

  FrameDecoder decoder(max_frame_bytes);
  std::array<std::uint8_t, 4096> buffer{};
  while (true) {
    Status readable = waitFor(fd, WaitDirection::kRead, deadline);
    if (!readable.ok()) {
      return readable;
    }

    const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (received < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return socketError(errno, "local IPC read failed");
    }
    if (received == 0) {
      return Status(PDCM_STATUS_UNAVAILABLE,
                    "local IPC peer closed the connection");
    }

    std::vector<Frame> frames;
    Status decoded = decoder.append(
        buffer.data(), static_cast<std::size_t>(received), &frames);
    if (!decoded.ok()) {
      return decoded;
    }
    if (frames.size() > 1) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "unexpected coalesced response frames");
    }
    if (frames.size() == 1) {
      *frame = std::move(frames.front());
      return Status::success();
    }
  }
}

} // namespace pdcm::ipc
