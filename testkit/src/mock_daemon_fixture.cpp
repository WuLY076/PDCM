#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "core/service_core.hpp"
#include "ipc/local_api_server.hpp"
#include "pdcm/testkit/manual_clock.hpp"
#include "pdcm/testkit/mock_metric_ids.hpp"
#include "pdcm/testkit/mock_provider.hpp"

int main(const int argc, char **const argv) {
  if (argc != 2 || argv == nullptr || argv[1] == nullptr) {
    return 2;
  }

  auto clock = std::make_shared<pdcm::testkit::ManualClock>(1000, 2000);
  pdcm::testkit::MockProviderConfig provider_config;
  provider_config.scenario = pdcm::testkit::MockScenario::kSingleDevice;

  pdcm::RuntimeConfig config;
  config.target = pdcm::TargetKind::kFpga;
  pdcm::TargetCatalog target_catalog =
      pdcm::TargetCatalog::blocked(config.target);
  target_catalog.health.front().provider_data_id =
      pdcm::testkit::kTestHeartbeatEvidenceId;
  pdcm::PdcmServiceCore core(
      config,
      std::make_unique<pdcm::testkit::MockProvider>(provider_config, clock),
      clock, target_catalog);
  if (!core.start().ok()) {
    return 3;
  }

  pdcm::ipc::LocalApiServer server(std::string(argv[1]), &core, config.limits);
  if (!server.start().ok()) {
    return 4;
  }

  for (int attempt = 0; attempt < 5000 && server.completedConnections() == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  const bool completed = server.completedConnections() != 0;
  const bool stopped = server.stop().ok() && core.stop().ok();
  return completed && stopped ? 0 : 5;
}
