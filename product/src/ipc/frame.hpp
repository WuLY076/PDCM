#ifndef PDCM_IPC_FRAME_HPP_
#define PDCM_IPC_FRAME_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/status.hpp"

namespace pdcm::ipc {

inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 0;
inline constexpr std::size_t kFrameHeaderBytes = 20;

enum class MessageType : std::uint16_t {
  kHelloRequest = 1,
  kHelloResponse = 2,
  kErrorResponse = 3,
  kVersionRequest = 4,
  kVersionResponse = 5,
  kEntityListRequest = 6,
  kEntityListResponse = 7,
  kCapabilityQueryRequest = 8,
  kCapabilityQueryResponse = 9,
  kHealthQueryRequest = 10,
  kHealthQueryResponse = 11,
};

struct Frame {
  std::uint16_t protocol_major{kProtocolMajor};
  std::uint16_t protocol_minor{kProtocolMinor};
  MessageType message_type{MessageType::kErrorResponse};
  std::uint16_t flags{0};
  std::uint64_t request_id{0};
  std::vector<std::uint8_t> payload;
};

[[nodiscard]] Status encodeFrame(const Frame &frame,
                                 std::size_t max_frame_bytes,
                                 std::vector<std::uint8_t> *output);

class FrameDecoder {
public:
  explicit FrameDecoder(std::size_t max_frame_bytes);

  Status append(const std::uint8_t *data, std::size_t size,
                std::vector<Frame> *frames);
  void reset() noexcept;

  [[nodiscard]] bool poisoned() const noexcept;
  [[nodiscard]] std::size_t bufferedBytes() const noexcept;

private:
  Status fail(pdcm_status_t status, const char *message);

  std::size_t max_frame_bytes_;
  std::vector<std::uint8_t> buffer_;
  bool poisoned_{false};
};

} // namespace pdcm::ipc

#endif // PDCM_IPC_FRAME_HPP_
