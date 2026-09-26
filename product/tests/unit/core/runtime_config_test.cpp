#include <chrono>

#include <gtest/gtest.h>

#include "core/runtime_config.hpp"

namespace pdcm {
namespace {

TEST(RuntimeConfigTest, AcceptsBoundedExplicitConfiguration) {
  RuntimeConfig config;
  config.target = TargetKind::kFpga;
  EXPECT_TRUE(config.validate().ok());
}

TEST(RuntimeConfigTest, RejectsUnknownTarget) {
  RuntimeConfig config;
  EXPECT_EQ(config.validate().code(), PDCM_STATUS_INVALID_ARGUMENT);
}

TEST(RuntimeConfigTest, RejectsUnboundedOrInvalidLimits) {
  RuntimeConfig config;
  config.target = TargetKind::kEmu;

  config.limits.max_sessions = 0;
  EXPECT_EQ(config.validate().code(), PDCM_STATUS_INVALID_ARGUMENT);

  config.limits.max_sessions = 1;
  config.limits.max_frame_bytes = 19;
  EXPECT_EQ(config.validate().code(), PDCM_STATUS_INVALID_ARGUMENT);

  config.limits.max_frame_bytes = 20;
  config.provider_call_timeout = std::chrono::milliseconds(0);
  EXPECT_EQ(config.validate().code(), PDCM_STATUS_INVALID_ARGUMENT);
}

} // namespace
} // namespace pdcm
