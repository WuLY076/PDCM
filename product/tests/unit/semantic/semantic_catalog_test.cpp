#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "semantic/semantic_catalog.hpp"

namespace pdcm {
namespace {

constexpr std::uint32_t kTestHeartbeatEvidenceId = 0xF0000004U;

ProviderEntity device(std::string incarnation = "boot-1",
                      std::string pdrv_version = "pdrv-1") {
  ProviderEntity entity;
  entity.stable_native_id = "native-0";
  entity.pci_bdf = "0000:01:00.0";
  entity.pdrv_version = std::move(pdrv_version);
  entity.incarnation = std::move(incarnation);
  entity.state = ProviderEntityState::kReady;
  entity.manageable = true;
  return entity;
}

ProviderDescriptor descriptor(std::vector<ProviderEntity> entities) {
  ProviderDescriptor value;
  value.provider_version = "provider-1";
  value.target = TargetKind::kFpga;
  value.state = ProviderState::kReady;
  value.detected_device_count = static_cast<std::uint32_t>(entities.size());
  value.entities = std::move(entities);
  value.capabilities.push_back(
      ProviderCapability{ProviderDataKind::kHeartbeatEvidence,
                         kTestHeartbeatEvidenceId, true, "TEST_SUPPORTED"});
  return value;
}

TargetCatalog testCatalog() {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.health.front().provider_data_id = kTestHeartbeatEvidenceId;
  return catalog;
}

TEST(TargetCatalogTest, KeepsMetricsExplicitlyBlockedAndHealthNarrow) {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  EXPECT_TRUE(catalog.validate().ok());
  EXPECT_EQ(catalog.metrics_status, MetricsCatalogStatus::kBlockedExternal);
  EXPECT_TRUE(catalog.metrics.empty());
  ASSERT_EQ(catalog.health.size(), 1);
  EXPECT_EQ(catalog.health.front().subsystem_id, kFirmwareHeartbeatHealthId);

  MetricDescriptor guessed;
  guessed.id = MetricId{1};
  guessed.name = "guessed_metric";
  guessed.unit = "count";
  guessed.min_period_ns = 1;
  guessed.default_period_ns = 1;
  guessed.freshness_ns = 1;
  guessed.semantic_version = 1;
  guessed.provider_mapping_approved = true;
  catalog.metrics.push_back(guessed);
  EXPECT_EQ(catalog.validate().code(), PDCM_STATUS_INVALID_ARGUMENT);

  catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.health.push_back(catalog.health.front());
  EXPECT_EQ(catalog.validate().code(), PDCM_STATUS_INVALID_ARGUMENT);
}

TEST(SemanticCatalogTest, CommitsZeroOneAndUnsupportedTopologyAtomically) {
  SemanticCatalog catalog(testCatalog());

  CatalogCommitResult zero = catalog.commit(descriptor({}));
  ASSERT_TRUE(zero.status.ok());
  EXPECT_TRUE(zero.committed);
  EXPECT_EQ(zero.catalog_generation, 1);
  EXPECT_TRUE(catalog.snapshot()->entities().empty());

  CatalogCommitResult one = catalog.commit(descriptor({device()}));
  ASSERT_TRUE(one.status.ok());
  ASSERT_EQ(catalog.snapshot()->entities().size(), 1);
  const EntityRecord entity = catalog.snapshot()->entities().front();
  EXPECT_EQ(entity.ref.id, EntityId{0});
  EXPECT_EQ(entity.ref.generation, 1);
  EXPECT_EQ(entity.native_id.status, ObservationStatus::kValid);
  EXPECT_EQ(entity.pci_bdf.value, "0000:01:00.0");

  CatalogCommitResult multiple = catalog.commit(descriptor(
      {device(), ProviderEntity{"native-1", "0000:02:00.0", "pdrv-1", "boot-1",
                                ProviderEntityState::kReady, true}}));
  EXPECT_EQ(multiple.status.code(), PDCM_STATUS_UNSUPPORTED);
  EXPECT_TRUE(multiple.committed);
  EXPECT_TRUE(catalog.snapshot()->topologyUnsupported());
  EXPECT_EQ(catalog.snapshot()->detectedDeviceCount(), 2);
  EXPECT_TRUE(catalog.snapshot()->entities().empty());
}

TEST(SemanticCatalogTest, RejectsMalformedDescriptorWithoutPublishing) {
  SemanticCatalog catalog(testCatalog());
  ASSERT_TRUE(catalog.commit(descriptor({device()})).status.ok());
  const std::shared_ptr<const CatalogView> before = catalog.snapshot();

  ProviderDescriptor malformed = descriptor({device()});
  malformed.entities.front().stable_native_id.clear();
  CatalogCommitResult rejected = catalog.commit(malformed);

  EXPECT_EQ(rejected.status.code(), PDCM_STATUS_INVALID_ARGUMENT);
  EXPECT_FALSE(rejected.committed);
  EXPECT_EQ(catalog.snapshot(), before);
}

TEST(SemanticCatalogTest, TracksCatalogAndEntityGenerationsSeparately) {
  SemanticCatalog catalog(testCatalog());
  ASSERT_TRUE(catalog.commit(descriptor({device()})).status.ok());
  const EntityRef original = catalog.snapshot()->entities().front().ref;

  ASSERT_TRUE(
      catalog.commit(descriptor({device("boot-1", "pdrv-2")})).status.ok());
  const EntityRef attribute_refresh =
      catalog.snapshot()->entities().front().ref;
  EXPECT_GT(catalog.snapshot()->generation(), 1);
  EXPECT_EQ(attribute_refresh.generation, original.generation);

  ASSERT_TRUE(catalog.commit(descriptor({})).status.ok());
  ASSERT_TRUE(catalog.commit(descriptor({device("boot-2")})).status.ok());
  const EntityRef reappeared = catalog.snapshot()->entities().front().ref;
  EXPECT_EQ(reappeared.id, original.id);
  EXPECT_GT(reappeared.generation, original.generation);
  EXPECT_EQ(catalog.snapshot()->resolve(original).status.code(),
            PDCM_STATUS_STALE_GENERATION);
  EXPECT_TRUE(catalog.snapshot()->resolve(reappeared).status.ok());
}

TEST(SemanticCatalogTest, DerivesCapabilitiesOnlyFromCatalogAndProviderFacts) {
  SemanticCatalog catalog(testCatalog());
  ASSERT_TRUE(catalog.commit(descriptor({device()})).status.ok());
  const std::shared_ptr<const CatalogView> view = catalog.snapshot();
  const EntityRef entity = view->entities().front().ref;

  CapabilityQueryResult queried = view->capabilities(entity);
  ASSERT_TRUE(queried.status.ok());
  ASSERT_TRUE(queried.capabilities.has_value());
  ASSERT_EQ(queried.capabilities->items.size(), 2);
  EXPECT_EQ(queried.capabilities->items[0].kind,
            CapabilityKind::kMetricsCatalog);
  EXPECT_FALSE(queried.capabilities->items[0].supported);
  EXPECT_EQ(queried.capabilities->items[0].reason,
            CapabilityReason::kCatalogBlockedExternal);
  EXPECT_EQ(queried.capabilities->items[1].kind, CapabilityKind::kHealth);
  EXPECT_TRUE(queried.capabilities->items[1].supported);
  EXPECT_EQ(queried.capabilities->items[1].reason,
            CapabilityReason::kSupported);
}

TEST(SemanticCatalogTest, ConcurrentReadersObserveCompleteSnapshots) {
  SemanticCatalog catalog(testCatalog());
  ASSERT_TRUE(catalog.commit(descriptor({})).status.ok());

  std::atomic<bool> stop{false};
  std::atomic<bool> invalid{false};
  std::thread reader([&] {
    while (!stop.load()) {
      const std::shared_ptr<const CatalogView> view = catalog.snapshot();
      if (view->entities().size() > 1 ||
          (view->entities().size() == 1 &&
           view->entities().front().ref.generation == 0)) {
        invalid.store(true);
      }
    }
  });

  for (int iteration = 0; iteration < 100; ++iteration) {
    ASSERT_TRUE(
        catalog
            .commit(descriptor(iteration % 2 == 0
                                   ? std::vector<ProviderEntity>{device()}
                                   : std::vector<ProviderEntity>{}))
            .status.ok());
  }
  stop.store(true);
  reader.join();
  EXPECT_FALSE(invalid.load());
}

} // namespace
} // namespace pdcm
