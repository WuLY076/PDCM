#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#ifndef PDCM_MOCK_DAEMON_PATH
#error "PDCM_MOCK_DAEMON_PATH must name the mock daemon executable"
#endif

#ifndef PDCM_CLI_PATH
#error "PDCM_CLI_PATH must name the pdcm-cli executable"
#endif

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    const std::string value = "/tmp/pdcm-cli-roundtrip-XXXXXX";
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

  bool valid() const noexcept { return !path_.empty(); }
  std::string socketPath() const { return path_ + "/pdcm.sock"; }

private:
  std::string path_;
};

class MockDaemonProcess {
public:
  ~MockDaemonProcess() {
    if (pid_ > 0) {
      (void)::kill(pid_, SIGTERM);
      (void)::waitpid(pid_, nullptr, 0);
    }
  }

  bool start(const std::string &endpoint) {
    pid_ = ::fork();
    if (pid_ == 0) {
      ::execl(PDCM_MOCK_DAEMON_PATH, PDCM_MOCK_DAEMON_PATH,
              endpoint.c_str(), static_cast<char *>(nullptr));
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

struct ProcessResult {
  int exit_code{-1};
  std::string output;
};

ProcessResult runHealthCli(const std::string &endpoint) {
  int descriptors[2] = {-1, -1};
  if (::pipe(descriptors) != 0) {
    return {};
  }
  const pid_t child = ::fork();
  if (child == 0) {
    (void)::close(descriptors[0]);
    if (::dup2(descriptors[1], STDOUT_FILENO) < 0 ||
        ::dup2(descriptors[1], STDERR_FILENO) < 0) {
      ::_exit(126);
    }
    (void)::close(descriptors[1]);
    ::execl(PDCM_CLI_PATH, PDCM_CLI_PATH, "health", "--format", "json",
            "--endpoint", endpoint.c_str(), static_cast<char *>(nullptr));
    ::_exit(127);
  }
  (void)::close(descriptors[1]);
  if (child < 0) {
    (void)::close(descriptors[0]);
    return {};
  }

  ProcessResult result;
  std::array<char, 4096> buffer{};
  for (;;) {
    const ssize_t count = ::read(descriptors[0], buffer.data(), buffer.size());
    if (count > 0) {
      result.output.append(buffer.data(), static_cast<std::size_t>(count));
    } else if (count == 0) {
      break;
    } else if (errno != EINTR) {
      break;
    }
  }
  (void)::close(descriptors[0]);
  int status = 0;
  if (::waitpid(child, &status, 0) == child && WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  }
  return result;
}

TEST(CliRoundTripTest, QueriesMockHealthThroughInstalledArchitecture) {
  TemporaryDirectory directory;
  ASSERT_TRUE(directory.valid());
  const std::string endpoint = directory.socketPath();

  MockDaemonProcess daemon;
  ASSERT_TRUE(daemon.start(endpoint));
  for (int attempt = 0; attempt < 500 && ::access(endpoint.c_str(), F_OK) != 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_EQ(::access(endpoint.c_str(), F_OK), 0);

  const ProcessResult result = runHealthCli(endpoint);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.output.find("\"status\":\"PARTIAL_RESULT\""),
            std::string::npos);
  EXPECT_NE(result.output.find("\"state\":\"UNKNOWN\""),
            std::string::npos);
  EXPECT_NE(result.output.find("\"code\":\"HEARTBEAT_MISSING\""),
            std::string::npos);
  EXPECT_EQ(result.output.find("PUBLIC_HEALTH_QUERY_PENDING"),
            std::string::npos);
  EXPECT_TRUE(daemon.waitForSuccess());
}

} // namespace
