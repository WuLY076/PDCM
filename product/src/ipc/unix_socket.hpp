#ifndef PDCM_IPC_UNIX_SOCKET_HPP_
#define PDCM_IPC_UNIX_SOCKET_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/clock.hpp"
#include "common/status.hpp"
#include "ipc/frame.hpp"

namespace pdcm::ipc {

class UniqueFd {
public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept;
  ~UniqueFd();

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept;
  UniqueFd &operator=(UniqueFd &&other) noexcept;

  [[nodiscard]] int get() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
  int release() noexcept;
  void reset(int fd = -1) noexcept;

private:
  int fd_{-1};
};

Status connectUnixSocket(const std::string &endpoint, MonotonicTime deadline,
                         UniqueFd *socket);
Status checkUnixSocketConnected(int fd);
Status writeAll(int fd, const std::uint8_t *data, std::size_t size,
                MonotonicTime deadline);
Status readOneFrame(int fd, Frame *frame, std::size_t max_frame_bytes,
                    MonotonicTime deadline);

} // namespace pdcm::ipc

#endif // PDCM_IPC_UNIX_SOCKET_HPP_
