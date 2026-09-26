#ifndef PDCM_DAEMON_DAEMON_CONFIG_HPP_
#define PDCM_DAEMON_DAEMON_CONFIG_HPP_

#include <iosfwd>
#include <string>

#include "common/status.hpp"
#include "core/runtime_config.hpp"

namespace pdcm::daemon {

struct DaemonConfig {
  RuntimeConfig runtime;
  std::string endpoint{"/run/pdcm/pdcm.sock"};
};

Status parseConfig(std::istream &input, DaemonConfig *config);
Status loadConfig(const std::string &path, DaemonConfig *config);

} // namespace pdcm::daemon

#endif // PDCM_DAEMON_DAEMON_CONFIG_HPP_
