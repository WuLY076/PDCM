#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "ipc/frame.hpp"
#include "proto/local/pdcm_local.pb.h"

namespace pdcm::ipc {
namespace {

Frame helloFrame() {
  local::v1::HelloRequest hello;
  hello.set_protocol_major(kProtocolMajor);
  hello.set_protocol_minor(kProtocolMinor);
  hello.set_client_library_version("0.1.0");
  hello.set_process_id(42);
  hello.set_nonce("0123456789abcdef");

  Frame frame;
  frame.message_type = MessageType::kHelloRequest;
  frame.request_id = 0x0102030405060708ULL;
  const std::string serialized = hello.SerializeAsString();
  frame.payload.assign(serialized.begin(), serialized.end());
  return frame;
}

TEST(FrameCodecTest, EncodesBigEndianHeaderAndProtobufPayload) {
  const Frame source = helloFrame();
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encodeFrame(source, 4096, &encoded).ok());
  ASSERT_GE(encoded.size(), kFrameHeaderBytes);

  EXPECT_EQ(encoded[4], 0);
  EXPECT_EQ(encoded[5], kProtocolMajor);
  EXPECT_EQ(encoded[12], 0x01);
  EXPECT_EQ(encoded[19], 0x08);

  FrameDecoder decoder(4096);
  std::vector<Frame> decoded;
  ASSERT_TRUE(decoder.append(encoded.data(), encoded.size(), &decoded).ok());
  ASSERT_EQ(decoded.size(), 1);
  EXPECT_EQ(decoded[0].request_id, source.request_id);
  EXPECT_EQ(decoded[0].message_type, MessageType::kHelloRequest);

  local::v1::HelloRequest parsed;
  ASSERT_TRUE(parsed.ParseFromArray(
      decoded[0].payload.data(), static_cast<int>(decoded[0].payload.size())));
  EXPECT_EQ(parsed.client_library_version(), "0.1.0");
  EXPECT_EQ(parsed.process_id(), 42);
}

TEST(FrameCodecTest, SupportsFragmentationAndCoalescing) {
  const Frame source = helloFrame();
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encodeFrame(source, 4096, &encoded).ok());

  FrameDecoder fragmented(4096);
  std::vector<Frame> fragmented_frames;
  for (const std::uint8_t byte : encoded) {
    ASSERT_TRUE(fragmented.append(&byte, 1, &fragmented_frames).ok());
  }
  ASSERT_EQ(fragmented_frames.size(), 1);

  std::vector<std::uint8_t> coalesced = encoded;
  coalesced.insert(coalesced.end(), encoded.begin(), encoded.end());
  FrameDecoder combined(4096);
  std::vector<Frame> combined_frames;
  ASSERT_TRUE(
      combined.append(coalesced.data(), coalesced.size(), &combined_frames)
          .ok());
  EXPECT_EQ(combined_frames.size(), 2);
}

TEST(FrameCodecTest, AcceptsEmptyInputWithoutDereferencingNull) {
  FrameDecoder decoder(4096);
  std::vector<Frame> decoded;
  EXPECT_TRUE(decoder.append(nullptr, 0, &decoded).ok());
  EXPECT_TRUE(decoded.empty());
  EXPECT_FALSE(decoder.poisoned());
}

TEST(FrameCodecTest, WaitsForIncompleteFrameWithoutEmittingData) {
  const Frame source = helloFrame();
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encodeFrame(source, 4096, &encoded).ok());

  FrameDecoder decoder(4096);
  std::vector<Frame> decoded;
  ASSERT_TRUE(
      decoder.append(encoded.data(), encoded.size() - 1, &decoded).ok());
  EXPECT_TRUE(decoded.empty());
  EXPECT_EQ(decoder.bufferedBytes(), encoded.size() - 1);
}

TEST(FrameCodecTest, RejectsMalformedAndOversizedFrames) {
  std::array<std::uint8_t, 4> short_header{0, 0, 0, 19};
  FrameDecoder malformed(4096);
  std::vector<Frame> decoded;
  EXPECT_EQ(malformed.append(short_header.data(), short_header.size(), &decoded)
                .code(),
            PDCM_STATUS_INVALID_ARGUMENT);
  EXPECT_TRUE(malformed.poisoned());

  Frame large = helloFrame();
  large.payload.resize(128);
  std::vector<std::uint8_t> encoded;
  EXPECT_EQ(encodeFrame(large, 64, &encoded).code(),
            PDCM_STATUS_RESOURCE_EXHAUSTED);

  std::array<std::uint8_t, 4> oversized_header{0, 0, 16, 1};
  FrameDecoder oversized(4096);
  EXPECT_EQ(
      oversized
          .append(oversized_header.data(), oversized_header.size(), &decoded)
          .code(),
      PDCM_STATUS_RESOURCE_EXHAUSTED);
}

TEST(FrameCodecTest, PreservesUnknownMessageTypeForRouterValidation) {
  Frame source = helloFrame();
  source.message_type = static_cast<MessageType>(65000);
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encodeFrame(source, 4096, &encoded).ok());

  FrameDecoder decoder(4096);
  std::vector<Frame> decoded;
  ASSERT_TRUE(decoder.append(encoded.data(), encoded.size(), &decoded).ok());
  ASSERT_EQ(decoded.size(), 1);
  EXPECT_EQ(static_cast<std::uint16_t>(decoded[0].message_type), 65000);
}

} // namespace
} // namespace pdcm::ipc
