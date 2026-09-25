#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include "pdcm/pdcm.h"

#ifndef PDCM_MOCK_DAEMON_PATH
#error "PDCM_MOCK_DAEMON_PATH must name the test daemon executable"
#endif

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    const std::string value = "/tmp/pdcm-roundtrip-XXXXXX";
    std::copy(value.begin(), value.end(), pattern.begin());
    char *const created = ::mkdtemp(pattern.data());
    if (created != nullptr) {
      path_ = created;
    }
  }

  ~TemporaryDirectory() {
    if (!path_.empty()) {
      (void)::unlink(socketPath().c_str());
      (void)::rmdir(path_.c_str());
    }
  }

  [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }

  [[nodiscard]] std::string socketPath() const { return path_ + "/pdcm.sock"; }

private:
  std::string path_;
};

class ChildProcess {
public:
  ~ChildProcess() {
    if (pid_ > 0) {
      (void)::kill(pid_, SIGTERM);
      (void)::waitpid(pid_, nullptr, 0);
    }
  }

  bool start(const std::string &endpoint) {
    pid_ = ::fork();
    if (pid_ == 0) {
      ::execl(PDCM_MOCK_DAEMON_PATH, PDCM_MOCK_DAEMON_PATH, endpoint.c_str(),
              static_cast<char *>(nullptr));
      ::_exit(127);
    }
    return pid_ > 0;
  }

  bool waitForSuccess() {
    for (int attempt = 0; attempt < 500; ++attempt) {
      int status = 0;
      const pid_t result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
      }
      if (result < 0) {
        pid_ = -1;
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

private:
  pid_t pid_{-1};
};

class PublicHandle {
public:
  ~PublicHandle() {
    if (value != nullptr) {
      (void)pdcm_close(&value);
    }
  }

  pdcm_handle_t *value{nullptr};
};

TEST(StandaloneRoundTripTest, PublicApiHandshakesWithMockDaemonProcess) {
  TemporaryDirectory directory;
  ASSERT_TRUE(directory.valid());
  const std::string endpoint = directory.socketPath();

  ChildProcess daemon;
  ASSERT_TRUE(daemon.start(endpoint));

  for (int attempt = 0; attempt < 500 && ::access(endpoint.c_str(), F_OK) != 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_EQ(::access(endpoint.c_str(), F_OK), 0);

  pdcm_open_options_t options = PDCM_OPEN_OPTIONS_INIT;
  options.mode = PDCM_MODE_STANDALONE;
  options.endpoint = endpoint.c_str();
  options.deadline_ns = UINT64_C(1000000000);

  PublicHandle handle;
  ASSERT_EQ(pdcm_open(&options, &handle.value), PDCM_STATUS_SUCCESS);
  ASSERT_NE(handle.value, nullptr);

  pdcm_version_info_t version = PDCM_VERSION_INFO_INIT;
  ASSERT_EQ(pdcm_version_get(handle.value, &version), PDCM_STATUS_SUCCESS);
  EXPECT_EQ(version.mode, PDCM_MODE_STANDALONE);
  EXPECT_EQ(version.core_state, PDCM_CORE_STATE_READY);
  EXPECT_EQ(version.provider_state, PDCM_PROVIDER_STATE_READY);
  EXPECT_EQ(version.detected_device_count, 1);
  EXPECT_EQ(version.catalog_generation, 1);
  EXPECT_GT(version.session_id, 0);
  std::size_t entity_count = 0;
  ASSERT_EQ(pdcm_entity_list(handle.value, nullptr, nullptr, &entity_count),
            PDCM_STATUS_SUCCESS);
  ASSERT_EQ(entity_count, 1);

  pdcm_entity_info_t entity = PDCM_ENTITY_INFO_INIT;
  std::size_t entity_capacity = 1;
  ASSERT_EQ(pdcm_entity_list(handle.value, nullptr, &entity, &entity_capacity),
            PDCM_STATUS_SUCCESS);
  ASSERT_EQ(entity_capacity, 1);
  EXPECT_EQ(entity.entity.kind, PDCM_ENTITY_KIND_DEVICE);
  EXPECT_EQ(entity.entity.pdcm_id, 0);
  EXPECT_EQ(entity.entity.generation, 1);
  EXPECT_EQ(entity.state, PDCM_ENTITY_STATE_READY);
  EXPECT_EQ(entity.item_status, PDCM_STATUS_SUCCESS);
  EXPECT_EQ(entity.native_id_status, PDCM_OBSERVATION_VALID);
  EXPECT_STREQ(entity.native_id, "mock-device-0");
  EXPECT_STREQ(entity.pci_bdf, "0000:01:00.0");
  EXPECT_STREQ(entity.pdrv_version, "mock-pdrv-1.0");

  pdcm_capability_set_t capabilities = PDCM_CAPABILITY_SET_INIT;
  ASSERT_EQ(pdcm_capability_query(handle.value, &entity.entity, &capabilities),
            PDCM_STATUS_BUFFER_TOO_SMALL);
  ASSERT_EQ(capabilities.item_count, 2);
  EXPECT_EQ(capabilities.catalog_generation, 1);

  std::array<pdcm_capability_item_t, 2> items{};
  for (pdcm_capability_item_t &item : items) {
    item.header.struct_size = sizeof(pdcm_capability_item_t);
    item.header.version = PDCM_STRUCT_VERSION_1;
  }
  capabilities.items = items.data();
  capabilities.item_capacity = items.size();
  ASSERT_EQ(pdcm_capability_query(handle.value, &entity.entity, &capabilities),
            PDCM_STATUS_SUCCESS);
  ASSERT_EQ(capabilities.item_count, 2);
  EXPECT_EQ(items[0].kind, PDCM_CAPABILITY_METRICS_CATALOG);
  EXPECT_EQ(items[0].supported, 0);
  EXPECT_EQ(items[0].reason, PDCM_CAPABILITY_REASON_CATALOG_BLOCKED_EXTERNAL);
  EXPECT_EQ(items[1].kind, PDCM_CAPABILITY_HEALTH);
  EXPECT_EQ(items[1].supported, 1);
  EXPECT_EQ(items[1].reason, PDCM_CAPABILITY_REASON_SUPPORTED);

  pdcm_health_request_t health_request = PDCM_HEALTH_REQUEST_INIT;
  health_request.entity = entity.entity;
  health_request.catalog_generation = capabilities.catalog_generation;
  pdcm_health_result_t health = PDCM_HEALTH_RESULT_INIT;
  EXPECT_EQ(pdcm_health_query(handle.value, &health_request, &health),
            PDCM_STATUS_PARTIAL_RESULT);
  EXPECT_EQ(health.subsystem_id,
            PDCM_HEALTH_SUBSYSTEM_FIRMWARE_HEARTBEAT);
  EXPECT_EQ(health.state, PDCM_HEALTH_STATE_UNKNOWN);
  EXPECT_EQ(health.item_status, PDCM_OBSERVATION_NOT_AVAILABLE);
  EXPECT_EQ(health.code, PDCM_HEALTH_CODE_HEARTBEAT_MISSING);
  EXPECT_EQ(health.catalog_generation, capabilities.catalog_generation);
  EXPECT_GT(health.evaluated_monotonic_time_ns, 0);
  EXPECT_NE(health.limitation[0], '\0');

  pdcm_entity_ref_t stale = entity.entity;
  ++stale.generation;
  pdcm_capability_set_t stale_result = PDCM_CAPABILITY_SET_INIT;
  EXPECT_EQ(pdcm_capability_query(handle.value, &stale, &stale_result),
            PDCM_STATUS_STALE_GENERATION);
  EXPECT_EQ(stale_result.item_count, 0);

  ASSERT_EQ(pdcm_close(&handle.value), PDCM_STATUS_SUCCESS);
  EXPECT_TRUE(daemon.waitForSuccess());
  EXPECT_NE(::access(endpoint.c_str(), F_OK), 0);
}

} // namespace
