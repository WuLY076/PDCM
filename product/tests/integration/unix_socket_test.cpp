#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <sys/socket.h>

#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"

namespace pdcm::ipc {
namespace {

MonotonicTime after(const std::chrono::milliseconds duration) {
  return std::chrono::time_point_cast<Nanoseconds>(
             std::chrono::steady_clock::now()) +
         duration;
}

TEST(UnixSocketTest, TransfersOneFramedMessageAndDetectsDisconnect) {
  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                         descriptors),
            0);
  UniqueFd sender(descriptors[0]);
  UniqueFd receiver(descriptors[1]);

  Frame expected;
  expected.message_type = MessageType::kVersionRequest;
  expected.request_id = 42;
  expected.payload = {1, 2, 3, 4};

  std::vector<std::uint8_t> bytes;
  ASSERT_TRUE(encodeFrame(expected, 4096, &bytes).ok());
  ASSERT_TRUE(writeAll(sender.get(), bytes.data(), bytes.size(),
                       after(std::chrono::milliseconds(100)))
                  .ok());

  Frame actual;
  ASSERT_TRUE(readOneFrame(receiver.get(), &actual, 4096,
                           after(std::chrono::milliseconds(100)))
                  .ok());
  EXPECT_EQ(actual.message_type, expected.message_type);
  EXPECT_EQ(actual.request_id, expected.request_id);
  EXPECT_EQ(actual.payload, expected.payload);
  EXPECT_TRUE(checkUnixSocketConnected(sender.get()).ok());

  receiver.reset();
  EXPECT_EQ(checkUnixSocketConnected(sender.get()).code(),
            PDCM_STATUS_UNAVAILABLE);
}

TEST(UnixSocketTest, HonorsExpiredReadDeadline) {
  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                         descriptors),
            0);
  UniqueFd sender(descriptors[0]);
  UniqueFd receiver(descriptors[1]);

  Frame frame;
  EXPECT_EQ(readOneFrame(receiver.get(), &frame, 4096,
                         after(std::chrono::milliseconds(-1)))
                .code(),
            PDCM_STATUS_TIMEOUT);
}

} // namespace
} // namespace pdcm::ipc
