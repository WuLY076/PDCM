#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <fstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include "pdcm/pdcm.h"

#ifndef PDCM_DAEMON_PATH
#error "PDCM_DAEMON_PATH must name the production daemon executable"
#endif

namespace {

class TemporaryDaemonFiles {
public:
  TemporaryDaemonFiles() {
    std::array<char, 64> pattern{};
    const std::string value = "/tmp/pdcm-daemon-test-XXXXXX";
    std::copy(value.begin(), value.end(), pattern.begin());
    char *const created = ::mkdtemp(pattern.data());
    if (created != nullptr) {
      directory_ = created;
      std::ofstream config(configPath());
      config << "target=fpga\n"
             << "endpoint=" << socketPath() << "\n"
             << "provider_call_timeout_ms=100\n"
             << "shutdown_grace_ms=1000\n"
             << "max_sessions=4\n"
             << "max_outstanding_requests_per_session=4\n"
             << "max_watches_per_session=4\n"
             << "max_subscriptions_per_session=4\n"
             << "max_frame_bytes=65536\n";
      valid_ = config.good();
    }
  }

  ~TemporaryDaemonFiles() {
    if (!directory_.empty()) {
      (void)::unlink(socketPath().c_str());
      (void)::unlink(configPath().c_str());
      (void)::rmdir(directory_.c_str());
    }
  }

  bool valid() const noexcept { return valid_; }
  std::string socketPath() const { return directory_ + "/pdcm.sock"; }
  std::string configPath() const { return directory_ + "/pdcm.conf"; }

private:
  std::string directory_;
  bool valid_{false};
};

class DaemonProcess {
public:
  ~DaemonProcess() {
    if (pid_ > 0) {
      (void)::kill(pid_, SIGKILL);
      (void)::waitpid(pid_, nullptr, 0);
    }
  }

  bool start(const std::string &config) {
    pid_ = ::fork();
    if (pid_ == 0) {
      ::execl(PDCM_DAEMON_PATH, PDCM_DAEMON_PATH, "--config", config.c_str(),
              static_cast<char *>(nullptr));
      ::_exit(127);
    }
    return pid_ > 0;
  }

  bool stop() {
    if (pid_ <= 0 || ::kill(pid_, SIGTERM) != 0) {
      return false;
    }
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

TEST(DaemonLifecycleTest, MissingProviderServesDegradedSessionAndStopsCleanly) {
  TemporaryDaemonFiles files;
  ASSERT_TRUE(files.valid());

  DaemonProcess daemon;
  ASSERT_TRUE(daemon.start(files.configPath()));
  for (int attempt = 0;
       attempt < 500 && ::access(files.socketPath().c_str(), F_OK) != 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_EQ(::access(files.socketPath().c_str(), F_OK), 0);

  pdcm_open_options_t options = PDCM_OPEN_OPTIONS_INIT;
  const std::string endpoint = files.socketPath();
  options.endpoint = endpoint.c_str();
  options.deadline_ns = UINT64_C(1000000000);
  pdcm_handle_t *handle = nullptr;
  ASSERT_EQ(pdcm_open(&options, &handle), PDCM_STATUS_SUCCESS);
  ASSERT_NE(handle, nullptr);

  pdcm_version_info_t version = PDCM_VERSION_INFO_INIT;
  ASSERT_EQ(pdcm_version_get(handle, &version), PDCM_STATUS_SUCCESS);
  EXPECT_EQ(version.core_state, PDCM_CORE_STATE_DEGRADED);
  EXPECT_EQ(version.provider_state, PDCM_PROVIDER_STATE_UNAVAILABLE);
  EXPECT_EQ(version.detail_status, PDCM_STATUS_UNAVAILABLE);
  EXPECT_EQ(version.detected_device_count, 0);
  EXPECT_EQ(pdcm_close(&handle), PDCM_STATUS_SUCCESS);

  EXPECT_TRUE(daemon.stop());
  EXPECT_NE(::access(files.socketPath().c_str(), F_OK), 0);
}

} // namespace
