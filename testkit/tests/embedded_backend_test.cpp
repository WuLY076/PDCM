#include <memory>

#include <gtest/gtest.h>

#include "client/embedded_backend.hpp"
#include "pdcm/testkit/manual_clock.hpp"
#include "pdcm/testkit/mock_provider.hpp"

namespace pdcm::testkit {
namespace {

TEST(EmbeddedBackendTest, ReusesCoreContractWithMockProvider) {
  auto clock = std::make_shared<ManualClock>(1000, 2000);
  MockProviderConfig provider_config;
  provider_config.scenario = MockScenario::kSingleDevice;

  RuntimeConfig config;
  config.target = TargetKind::kFpga;
  EmbeddedBackend backend(
      config, std::make_unique<MockProvider>(provider_config, clock), clock);

  ASSERT_TRUE(backend.start().ok());

  BackendVersion version;
  ASSERT_TRUE(backend.version(&version).ok());
  EXPECT_EQ(version.mode, BackendMode::kEmbedded);
  EXPECT_EQ(version.core.state, CoreState::kReady);
  EXPECT_EQ(version.core.provider_state, ProviderState::kReady);
  EXPECT_EQ(version.core.detected_device_count, 1);
  EXPECT_EQ(version.core.catalog_generation, 1);
  EXPECT_GT(version.session_id, 0);
  EntityListResult discovered = backend.entities(EntityKind::kDevice);
  ASSERT_TRUE(discovered.status.ok());
  ASSERT_EQ(discovered.entities.size(), 1);
  const EntityRef entity = discovered.entities.front().ref;
  EXPECT_EQ(discovered.entities.front().native_id.value, "mock-device-0");

  CapabilityQueryResult capabilities = backend.capabilities(entity);
  ASSERT_TRUE(capabilities.status.ok());
  ASSERT_TRUE(capabilities.capabilities.has_value());
  ASSERT_EQ(capabilities.capabilities->items.size(), 2);
  EXPECT_EQ(capabilities.capabilities->items.front().reason,
            CapabilityReason::kCatalogBlockedExternal);

  EntityRef stale = entity;
  ++stale.generation;
  EXPECT_EQ(backend.capabilities(stale).status.code(),
            PDCM_STATUS_STALE_GENERATION);

  EXPECT_TRUE(backend.close().ok());
  EXPECT_EQ(backend.version(&version).code(), PDCM_STATUS_NOT_INITIALIZED);
}

} // namespace
} // namespace pdcm::testkit
