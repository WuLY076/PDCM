#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "core/session_manager.hpp"
#include "ipc/handshake.hpp"
#include "pdcm_local.pb.h"

namespace pdcm {
namespace {

ResourceLimits smallLimits() {
  ResourceLimits limits;
  limits.max_sessions = 2;
  limits.max_outstanding_requests_per_session = 2;
  limits.max_watches_per_session = 1;
  limits.max_subscriptions_per_session = 1;
  limits.max_frame_bytes = 4096;
  return limits;
}

ipc::Frame helloFrame(std::uint16_t major, std::uint64_t process_id) {
  local::v1::HelloRequest hello;
  hello.set_protocol_major(major);
  hello.set_protocol_minor(7);
  hello.set_client_library_version("0.1.0");
  hello.set_process_id(process_id);
  hello.set_nonce("0123456789abcdef");

  ipc::Frame frame;
  frame.protocol_major = major;
  frame.protocol_minor = 7;
  frame.message_type = ipc::MessageType::kHelloRequest;
  frame.request_id = 99;
  const std::string serialized = hello.SerializeAsString();
  frame.payload.assign(serialized.begin(), serialized.end());
  return frame;
}

CoreSnapshot readyCore() {
  CoreSnapshot core;
  core.state = CoreState::kReady;
  core.provider_state = ProviderState::kReady;
  core.detail_status = PDCM_STATUS_SUCCESS;
  core.detected_device_count = 1;
  core.catalog_generation = 3;
  return core;
}

TEST(SessionManagerTest, EnforcesPerSessionQuotasAndCleanup) {
  ResourceLimits limits = smallLimits();
  SessionManager sessions(limits);
  SessionCreateResult created =
      sessions.beginHandshake(PeerIdentity{1000, 1000, 42});
  ASSERT_TRUE(created.status.ok());
  ASSERT_TRUE(sessions.activate(created.id).ok());

  EXPECT_TRUE(sessions.reserveRequest(created.id).ok());
  EXPECT_TRUE(sessions.reserveRequest(created.id).ok());
  EXPECT_EQ(sessions.reserveRequest(created.id).code(),
            PDCM_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_TRUE(sessions.releaseRequest(created.id).ok());

  EXPECT_TRUE(sessions.reserveWatch(created.id).ok());
  EXPECT_EQ(sessions.reserveWatch(created.id).code(),
            PDCM_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_TRUE(sessions.reserveSubscription(created.id).ok());

  ASSERT_TRUE(sessions.snapshot(created.id).has_value());
  EXPECT_EQ(sessions.snapshot(created.id)->counters.outstanding_requests, 1);
  EXPECT_TRUE(sessions.close(created.id).ok());
  EXPECT_FALSE(sessions.snapshot(created.id).has_value());
  EXPECT_EQ(sessions.size(), 0);
}

TEST(SessionManagerTest, DrainingRejectsNewResourcesAndSessions) {
  SessionManager sessions(smallLimits());
  SessionCreateResult created =
      sessions.beginHandshake(PeerIdentity{1000, 1000, 42});
  ASSERT_TRUE(created.status.ok());
  ASSERT_TRUE(sessions.activate(created.id).ok());

  EXPECT_TRUE(sessions.beginDraining().ok());
  EXPECT_EQ(sessions.snapshot(created.id)->state, SessionState::kDraining);
  EXPECT_EQ(sessions.reserveRequest(created.id).code(),
            PDCM_STATUS_UNAVAILABLE);
  EXPECT_EQ(sessions.beginHandshake(PeerIdentity{}).status.code(),
            PDCM_STATUS_UNAVAILABLE);

  sessions.closeAll();
  EXPECT_EQ(sessions.size(), 0);
}

TEST(HandshakeTest, ActivatesSessionAndReturnsNegotiatedState) {
  ResourceLimits limits = smallLimits();
  SessionManager sessions(limits);
  ipc::HandshakeHandler handler(&sessions);
  const PeerIdentity peer{1000, 1000, 42};

  ipc::HandshakeResult result = handler.handle(
      helloFrame(ipc::kProtocolMajor, 42), readyCore(), limits, peer);
  ASSERT_TRUE(result.status.ok());
  ASSERT_TRUE(result.session_id.has_value());
  EXPECT_FALSE(result.close_connection);
  EXPECT_EQ(result.response.message_type, ipc::MessageType::kHelloResponse);
  EXPECT_EQ(sessions.size(), 1);
  EXPECT_EQ(sessions.snapshot(*result.session_id)->state,
            SessionState::kActive);

  local::v1::HelloResponse response;
  ASSERT_TRUE(response.ParseFromArray(
      result.response.payload.data(),
      static_cast<int>(result.response.payload.size())));
  EXPECT_EQ(response.protocol_major(), ipc::kProtocolMajor);
  EXPECT_EQ(response.protocol_minor(), ipc::kProtocolMinor);
  EXPECT_EQ(response.core_state(), local::v1::CORE_STATE_READY);
  EXPECT_EQ(response.provider_state(), local::v1::PROVIDER_STATE_READY);
  EXPECT_EQ(response.catalog_generation(), 3);
  EXPECT_EQ(response.limits().max_watches(), limits.max_watches_per_session);
}

TEST(HandshakeTest, RejectsIncompatibleOrMalformedHelloWithoutSession) {
  ResourceLimits limits = smallLimits();
  SessionManager sessions(limits);
  ipc::HandshakeHandler handler(&sessions);
  const PeerIdentity peer{1000, 1000, 42};

  ipc::HandshakeResult incompatible =
      handler.handle(helloFrame(2, 42), readyCore(), limits, peer);
  EXPECT_EQ(incompatible.status.code(),
            PDCM_STATUS_PROTOCOL_INCOMPATIBLE);
  EXPECT_TRUE(incompatible.close_connection);
  EXPECT_EQ(sessions.size(), 0);

  ipc::Frame malformed = helloFrame(ipc::kProtocolMajor, 42);
  malformed.payload = {0xFF, 0xFF, 0xFF};
  ipc::HandshakeResult malformed_result =
      handler.handle(malformed, readyCore(), limits, peer);
  EXPECT_EQ(malformed_result.status.code(), PDCM_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(sessions.size(), 0);

  ipc::Frame wrong_type = helloFrame(ipc::kProtocolMajor, 42);
  wrong_type.message_type = ipc::MessageType::kVersionRequest;
  EXPECT_EQ(handler.handle(wrong_type, readyCore(), limits, peer).status.code(),
            PDCM_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(sessions.size(), 0);
}

TEST(HandshakeTest, RejectsClaimedPidMismatchAndSessionExhaustion) {
  ResourceLimits limits = smallLimits();
  limits.max_sessions = 1;
  SessionManager sessions(limits);
  ipc::HandshakeHandler handler(&sessions);
  const PeerIdentity peer{1000, 1000, 42};

  EXPECT_EQ(
      handler
          .handle(helloFrame(ipc::kProtocolMajor, 7), readyCore(), limits, peer)
          .status.code(),
      PDCM_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(sessions.size(), 0);

  ASSERT_TRUE(handler
                  .handle(helloFrame(ipc::kProtocolMajor, 42), readyCore(),
                          limits, peer)
                  .status.ok());
  ipc::HandshakeResult exhausted = handler.handle(
      helloFrame(ipc::kProtocolMajor, 42), readyCore(), limits, peer);
  EXPECT_EQ(exhausted.status.code(), PDCM_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_EQ(sessions.size(), 1);
}

} // namespace
} // namespace pdcm
