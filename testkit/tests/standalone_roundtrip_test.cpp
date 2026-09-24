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

  ASSERT_EQ(pdcm_close(&handle.value), PDCM_STATUS_SUCCESS);
  EXPECT_TRUE(daemon.waitForSuccess());
  EXPECT_NE(::access(endpoint.c_str(), F_OK), 0);
}

} // namespace
