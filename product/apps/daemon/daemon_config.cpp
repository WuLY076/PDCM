#include "daemon/daemon_config.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <istream>
#include <limits>
#include <string>
#include <string_view>

namespace pdcm::daemon {
namespace {

constexpr std::uint64_t kMaximumMilliseconds = UINT64_C(60000);
constexpr std::size_t kMaximumLineBytes = 4096;

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                            value.front() == '\r')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                            value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return value;
}

bool unsignedValue(const std::string_view text, std::uint64_t *const output) {
  if (text.empty()) {
    return false;
  }
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), *output);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

template <typename Value>
bool bounded(const std::string_view text, Value *const output) {
  std::uint64_t parsed = 0;
  if (!unsignedValue(text, &parsed) || parsed == 0 ||
      parsed > static_cast<std::uint64_t>(std::numeric_limits<Value>::max())) {
    return false;
  }
  *output = static_cast<Value>(parsed);
  return true;
}

Status apply(const std::string_view key, const std::string_view value,
             DaemonConfig *const config) {
  if (key == "target") {
    if (value == "fpga") {
      config->runtime.target = TargetKind::kFpga;
    } else if (value == "emu") {
      config->runtime.target = TargetKind::kEmu;
    } else {
      return Status(PDCM_STATUS_INVALID_ARGUMENT, "target must be fpga or emu");
    }
    return Status::success();
  }
  if (key == "endpoint") {
    if (value.empty() || value.front() != '/' || value.size() > 107) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "endpoint must be an absolute bounded UDS path");
    }
    config->endpoint.assign(value);
    return Status::success();
  }

  std::uint64_t number = 0;
  if (key == "provider_call_timeout_ms" || key == "shutdown_grace_ms") {
    if (!unsignedValue(value, &number) || number > kMaximumMilliseconds ||
        number == 0) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "daemon timeout must be between 1 and 60000 ms");
    }
    if (key == "provider_call_timeout_ms") {
      config->runtime.provider_call_timeout = std::chrono::milliseconds(number);
    } else {
      config->runtime.shutdown_grace = std::chrono::milliseconds(number);
    }
    return Status::success();
  }

  std::size_t *destination = nullptr;
  if (key == "max_sessions") {
    destination = &config->runtime.limits.max_sessions;
  } else if (key == "max_outstanding_requests_per_session") {
    destination = &config->runtime.limits.max_outstanding_requests_per_session;
  } else if (key == "max_watches_per_session") {
    destination = &config->runtime.limits.max_watches_per_session;
  } else if (key == "max_subscriptions_per_session") {
    destination = &config->runtime.limits.max_subscriptions_per_session;
  } else if (key == "max_frame_bytes") {
    destination = &config->runtime.limits.max_frame_bytes;
  } else {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "unknown daemon configuration key");
  }
  if (!bounded(value, destination)) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "daemon resource limit must be positive");
  }
  return Status::success();
}

} // namespace

Status parseConfig(std::istream &input, DaemonConfig *const config) {
  if (config == nullptr) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "daemon config output must not be null");
  }
  DaemonConfig candidate;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.size() > kMaximumLineBytes) {
      return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                    "daemon configuration line is too long");
    }
    std::string_view view = trim(line);
    if (view.empty() || view.front() == '#') {
      continue;
    }
    const std::size_t separator = view.find('=');
    if (separator == std::string_view::npos) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "daemon configuration entry has no separator");
    }
    const std::string_view key = trim(view.substr(0, separator));
    const std::string_view value = trim(view.substr(separator + 1));
    if (key.empty() || value.empty()) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "daemon configuration entry is empty");
    }
    const Status status = apply(key, value, &candidate);
    if (!status.ok()) {
      return Status(status.code(),
                    "invalid daemon configuration entry at line " +
                        std::to_string(line_number));
    }
  }
  if (!input.eof() && input.fail()) {
    return Status(PDCM_STATUS_INTERNAL,
                  "daemon configuration could not be read");
  }
  const Status valid = candidate.runtime.validate();
  if (!valid.ok()) {
    return valid;
  }
  *config = std::move(candidate);
  return Status::success();
}

Status loadConfig(const std::string &path, DaemonConfig *const config) {
  if (path.empty() || path.front() != '/') {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "daemon config path must be absolute");
  }
  std::ifstream input(path);
  if (!input.is_open()) {
    return Status(PDCM_STATUS_NOT_FOUND,
                  "daemon configuration file is unavailable");
  }
  return parseConfig(input, config);
}

} // namespace pdcm::daemon
