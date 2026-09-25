#include <csignal>
#include <iostream>
#include <memory>
#include <string>

#include <pthread.h>

#include "common/clock.hpp"
#include "core/service_core.hpp"
#include "daemon/daemon_config.hpp"
#include "ipc/local_api_server.hpp"
#include "provider/unavailable_provider.hpp"

namespace {

int usage() {
  std::cerr << "Usage: pdcm-daemon [--config ABSOLUTE_PATH] [--check-config]\n";
  return 2;
}

} // namespace

int main(const int argc, char **const argv) {
  std::string config_path{"/etc/pdcm/pdcm.conf"};
  bool check_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--config") {
      if (++index >= argc) {
        return usage();
      }
      config_path = argv[index];
    } else if (argument == "--check-config") {
      check_only = true;
    } else if (argument == "--help" || argument == "-h") {
      std::cout
          << "Usage: pdcm-daemon [--config ABSOLUTE_PATH] [--check-config]\n";
      return 0;
    } else {
      return usage();
    }
  }

  pdcm::daemon::DaemonConfig config;
  const pdcm::Status loaded = pdcm::daemon::loadConfig(config_path, &config);
  if (!loaded.ok()) {
    std::cerr << "pdcm-daemon: configuration validation failed\n";
    return 2;
  }
  if (check_only) {
    return 0;
  }

  sigset_t signals;
  if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGINT) != 0 ||
      ::sigaddset(&signals, SIGTERM) != 0 ||
      ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
    std::cerr << "pdcm-daemon: signal setup failed\n";
    return 8;
  }

  auto clock = std::make_shared<pdcm::SystemClock>();
  pdcm::PdcmServiceCore core(
      config.runtime, std::make_unique<pdcm::UnavailableProvider>(), clock);
  if (!core.start().ok()) {
    std::cerr << "pdcm-daemon: core startup failed\n";
    return 8;
  }

  pdcm::ipc::LocalApiServer server(config.endpoint, &core,
                                   config.runtime.limits);
  if (!server.start().ok()) {
    (void)core.stop();
    std::cerr << "pdcm-daemon: local API startup failed\n";
    return 8;
  }

  int received = 0;
  const int waited = ::sigwait(&signals, &received);
  const bool stopped = server.stop().ok() && core.stop().ok();
  return waited == 0 && stopped ? 0 : 8;
}
