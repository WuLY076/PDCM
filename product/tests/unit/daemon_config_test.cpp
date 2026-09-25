#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "daemon/daemon_config.hpp"

namespace pdcm::daemon {
namespace {

TEST(DaemonConfigTest, ParsesExplicitBoundedConfiguration) {
  std::istringstream input("target=fpga\n"
                           "endpoint=/tmp/pdcm.sock\n"
                           "provider_call_timeout_ms=250\n"
                           "shutdown_grace_ms=500\n"
                           "max_sessions=8\n"
                           "max_outstanding_requests_per_session=4\n"
                           "max_watches_per_session=5\n"
                           "max_subscriptions_per_session=6\n"
                           "max_frame_bytes=4096\n");
  DaemonConfig config;
  ASSERT_TRUE(parseConfig(input, &config).ok());
  EXPECT_EQ(config.runtime.target, TargetKind::kFpga);
  EXPECT_EQ(config.endpoint, "/tmp/pdcm.sock");
  EXPECT_EQ(config.runtime.provider_call_timeout,
            std::chrono::milliseconds(250));
  EXPECT_EQ(config.runtime.limits.max_sessions, 8);
  EXPECT_EQ(config.runtime.limits.max_frame_bytes, 4096);
}

TEST(DaemonConfigTest, RejectsUnknownUnsafeAndUnboundedInputsAtomically) {
  for (const char *const text :
       {"target=unknown\n", "endpoint=relative.sock\n",
        "provider_library=/tmp/libevil.so\n", "max_sessions=0\n",
        "provider_call_timeout_ms=60001\n", "broken\n"}) {
    std::istringstream input(text);
    DaemonConfig config;
    config.endpoint = "/unchanged.sock";
    EXPECT_FALSE(parseConfig(input, &config).ok()) << text;
    EXPECT_EQ(config.endpoint, "/unchanged.sock");
  }
}

TEST(DaemonConfigTest, RequiresExplicitTarget) {
  std::istringstream input("endpoint=/tmp/pdcm.sock\n");
  DaemonConfig config;
  EXPECT_EQ(parseConfig(input, &config).code(), PDCM_STATUS_INVALID_ARGUMENT);
}

} // namespace
} // namespace pdcm::daemon
