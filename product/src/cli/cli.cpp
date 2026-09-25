#include "cli/cli.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "pdcm/pdcm.h"

namespace pdcm::cli {
namespace {

constexpr std::uint64_t kSecond = UINT64_C(1000000000);
constexpr std::uint64_t kMaximumDuration = UINT64_C(60000000000);

enum class Command { kHelp, kVersion, kDiscovery, kDmon, kHealth };
enum class Format { kHuman, kJson };

struct Options {
  Command command{Command::kHelp};
  Format format{Format::kHuman};
  pdcm_mode_t mode{PDCM_MODE_STANDALONE};
  std::string endpoint{"/run/pdcm/pdcm.sock"};
  std::uint64_t timeout_ns{kSecond};
  std::optional<std::uint64_t> device;
  std::vector<std::string> metrics;
  std::uint64_t count{1};
  std::uint64_t period_ns{kSecond};
  bool fresh{false};
  bool verbose{false};
  bool command_help{false};
};

struct ParseResult {
  Options options;
  std::string error;
};

class Handle {
public:
  ~Handle() {
    if (value_ != nullptr) {
      (void)pdcm_close(&value_);
    }
  }
  Handle() = default;
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  pdcm_handle_t **out() noexcept { return &value_; }
  pdcm_handle_t *get() const noexcept { return value_; }

private:
  pdcm_handle_t *value_{nullptr};
};

const char *statusName(const pdcm_status_t status) {
  switch (status) {
  case PDCM_STATUS_SUCCESS:
    return "SUCCESS";
  case PDCM_STATUS_PARTIAL_RESULT:
    return "PARTIAL_RESULT";
  case PDCM_STATUS_INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case PDCM_STATUS_UNSUPPORTED:
    return "UNSUPPORTED";
  case PDCM_STATUS_NOT_INITIALIZED:
    return "NOT_INITIALIZED";
  case PDCM_STATUS_UNAVAILABLE:
    return "UNAVAILABLE";
  case PDCM_STATUS_PERMISSION_DENIED:
    return "PERMISSION_DENIED";
  case PDCM_STATUS_TIMEOUT:
    return "TIMEOUT";
  case PDCM_STATUS_RESOURCE_EXHAUSTED:
    return "RESOURCE_EXHAUSTED";
  case PDCM_STATUS_BUFFER_TOO_SMALL:
    return "BUFFER_TOO_SMALL";
  case PDCM_STATUS_NOT_FOUND:
    return "NOT_FOUND";
  case PDCM_STATUS_STALE_GENERATION:
    return "STALE_GENERATION";
  case PDCM_STATUS_INTERNAL:
    return "INTERNAL";
  }
  return "INTERNAL";
}

int exitCode(const pdcm_status_t status) {
  switch (status) {
  case PDCM_STATUS_SUCCESS:
    return 0;
  case PDCM_STATUS_PARTIAL_RESULT:
    return 1;
  case PDCM_STATUS_INVALID_ARGUMENT:
    return 2;
  case PDCM_STATUS_UNSUPPORTED:
  case PDCM_STATUS_NOT_FOUND:
  case PDCM_STATUS_STALE_GENERATION:
    return 3;
  case PDCM_STATUS_PERMISSION_DENIED:
    return 4;
  case PDCM_STATUS_NOT_INITIALIZED:
  case PDCM_STATUS_UNAVAILABLE:
    return 5;
  case PDCM_STATUS_TIMEOUT:
  case PDCM_STATUS_RESOURCE_EXHAUSTED:
  case PDCM_STATUS_BUFFER_TOO_SMALL:
    return 6;
  case PDCM_STATUS_INTERNAL:
    return 8;
  }
  return 8;
}

const char *coreState(const std::uint32_t state) {
  switch (state) {
  case PDCM_CORE_STATE_READY:
    return "READY";
  case PDCM_CORE_STATE_DEGRADED:
    return "DEGRADED";
  case PDCM_CORE_STATE_FAILED:
    return "FAILED";
  case PDCM_CORE_STATE_STOPPED:
    return "STOPPED";
  default:
    return "UNKNOWN";
  }
}

const char *providerState(const std::uint32_t state) {
  switch (state) {
  case PDCM_PROVIDER_STATE_READY:
    return "READY";
  case PDCM_PROVIDER_STATE_UNAVAILABLE:
    return "UNAVAILABLE";
  case PDCM_PROVIDER_STATE_SHUTDOWN:
    return "SHUTDOWN";
  default:
    return "UNKNOWN";
  }
}

bool unsignedValue(const std::string_view text, std::uint64_t *const output) {
  if (text.empty()) {
    return false;
  }
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), *output);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool duration(const std::string_view text, std::uint64_t *const output) {
  std::uint64_t scale = 0;
  std::string_view number;
  if (text.size() > 2 && text.substr(text.size() - 2) == "ms") {
    scale = UINT64_C(1000000);
    number = text.substr(0, text.size() - 2);
  } else if (text.size() > 1 && text.back() == 's') {
    scale = kSecond;
    number = text.substr(0, text.size() - 1);
  } else {
    return false;
  }
  std::uint64_t value = 0;
  if (!unsignedValue(number, &value) || value == 0 ||
      value > kMaximumDuration / scale) {
    return false;
  }
  *output = value * scale;
  return true;
}

std::optional<Command> command(const std::string_view value) {
  if (value == "help" || value == "--help" || value == "-h") {
    return Command::kHelp;
  }
  if (value == "version") {
    return Command::kVersion;
  }
  if (value == "discovery") {
    return Command::kDiscovery;
  }
  if (value == "dmon") {
    return Command::kDmon;
  }
  if (value == "health") {
    return Command::kHealth;
  }
  return std::nullopt;
}

ParseResult parse(const std::vector<std::string> &arguments) {
  ParseResult result;
  if (arguments.empty()) {
    return result;
  }
  const std::optional<Command> selected = command(arguments.front());
  if (!selected.has_value()) {
    result.error = "unknown command: " + arguments.front();
    return result;
  }
  result.options.command = *selected;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string &argument = arguments[index];
    auto take = [&](const char *name) -> const std::string * {
      if (++index >= arguments.size()) {
        result.error = std::string("missing value for ") + name;
        return nullptr;
      }
      return &arguments[index];
    };
    if (argument == "--help" || argument == "-h") {
      result.options.command_help = true;
    } else if (argument == "--verbose") {
      result.options.verbose = true;
    } else if (argument == "--fresh") {
      result.options.fresh = true;
    } else if (argument == "--format") {
      const std::string *value = take("--format");
      if (value == nullptr) {
        return result;
      }
      if (*value == "human") {
        result.options.format = Format::kHuman;
      } else if (*value == "json") {
        result.options.format = Format::kJson;
      } else {
        result.error = "--format must be human or json";
        return result;
      }
    } else if (argument == "--mode") {
      const std::string *value = take("--mode");
      if (value == nullptr) {
        return result;
      }
      if (*value == "standalone") {
        result.options.mode = PDCM_MODE_STANDALONE;
      } else if (*value == "embedded") {
        result.options.mode = PDCM_MODE_EMBEDDED;
      } else {
        result.error = "--mode must be standalone or embedded";
        return result;
      }
    } else if (argument == "--endpoint") {
      const std::string *value = take("--endpoint");
      if (value == nullptr) {
        return result;
      }
      if (value->empty() || value->front() != '/') {
        result.error = "--endpoint must be an absolute path";
        return result;
      }
      result.options.endpoint = *value;
    } else if (argument == "--timeout") {
      const std::string *value = take("--timeout");
      if (value == nullptr || !duration(*value, &result.options.timeout_ns)) {
        if (result.error.empty()) {
          result.error = "--timeout must be between 1ms and 60s";
        }
        return result;
      }
    } else if (argument == "--device") {
      const std::string *value = take("--device");
      std::uint64_t id = 0;
      if (value == nullptr || !unsignedValue(*value, &id)) {
        if (result.error.empty()) {
          result.error = "--device must be an unsigned ID";
        }
        return result;
      }
      result.options.device = id;
    } else if (argument == "--metric") {
      const std::string *value = take("--metric");
      if (value == nullptr || value->empty()) {
        if (result.error.empty()) {
          result.error = "--metric must not be empty";
        }
        return result;
      }
      result.options.metrics.push_back(*value);
    } else if (argument == "--count") {
      const std::string *value = take("--count");
      if (value == nullptr || !unsignedValue(*value, &result.options.count) ||
          result.options.count == 0) {
        if (result.error.empty()) {
          result.error = "--count must be positive";
        }
        return result;
      }
    } else if (argument == "--period") {
      const std::string *value = take("--period");
      if (value == nullptr || !duration(*value, &result.options.period_ns)) {
        if (result.error.empty()) {
          result.error = "--period must be between 1ms and 60s";
        }
        return result;
      }
    } else {
      result.error = "unknown option: " + argument;
      return result;
    }
  }
  if (result.options.command != Command::kDmon &&
      (!result.options.metrics.empty() || result.options.count != 1 ||
       result.options.period_ns != kSecond)) {
    result.error = "dmon options are only valid for dmon";
  } else if (result.options.command != Command::kDmon &&
             result.options.command != Command::kHealth &&
             result.options.fresh) {
    result.error = "--fresh is only valid for dmon or health";
  }
  return result;
}

const char *commandName(const Command value) {
  switch (value) {
  case Command::kHelp:
    return "help";
  case Command::kVersion:
    return "version";
  case Command::kDiscovery:
    return "discovery";
  case Command::kDmon:
    return "dmon";
  case Command::kHealth:
    return "health";
  }
  return "help";
}

void help(const std::optional<Command> selected, std::ostream &output) {
  if (!selected.has_value() || *selected == Command::kHelp) {
    output << "Usage: pdcm-cli COMMAND [OPTIONS]\n"
              "Commands:\n"
              "  version    Show component versions and status\n"
              "  discovery  Show the zero-or-one managed Device\n"
              "  dmon       Stream approved P0 metrics\n"
              "  health     Show firmware heartbeat health\n"
              "  help       Show this help\n"
              "Global options:\n"
              "  --format human|json\n"
              "  --timeout DURATION (1ms..60s, default 1s)\n"
              "  --endpoint ABSOLUTE_PATH\n"
              "  --mode standalone|embedded\n"
              "  --device ID\n"
              "  --verbose\n";
    return;
  }
  output << "Usage: pdcm-cli " << commandName(*selected);
  if (*selected == Command::kDmon) {
    output << " [--metric NAME_OR_ID] [--count N] [--period DURATION]"
              " [--fresh]";
  } else if (*selected == Command::kHealth) {
    output << " [--fresh]";
  }
  output << " [GLOBAL OPTIONS]\n"
            "P0 manages one Device; multiple Devices are unsupported.\n";
  if (*selected == Command::kDmon) {
    output << "Metrics come only from the approved Catalog; the production "
              "Catalog is currently BLOCKED_EXTERNAL.\n";
  }
}

pdcm_status_t open(const Options &options, Handle *const handle) {
  pdcm_open_options_t request = PDCM_OPEN_OPTIONS_INIT;
  request.mode = options.mode;
  request.endpoint =
      options.mode == PDCM_MODE_EMBEDDED ? nullptr : options.endpoint.c_str();
  request.deadline_ns = options.timeout_ns;
  request.target = options.mode == PDCM_MODE_EMBEDDED ? PDCM_TARGET_FPGA
                                                      : PDCM_TARGET_UNKNOWN;
  return pdcm_open(&request, handle->out());
}

pdcm_status_t entities(pdcm_handle_t *const handle,
                       std::vector<pdcm_entity_info_t> *const output) {
  std::size_t count = 0;
  pdcm_status_t status = pdcm_entity_list(handle, nullptr, nullptr, &count);
  if (status != PDCM_STATUS_SUCCESS || count == 0) {
    return status;
  }
  if (count > 1) {
    return PDCM_STATUS_UNSUPPORTED;
  }
  output->assign(count, PDCM_ENTITY_INFO_INIT);
  status = pdcm_entity_list(handle, nullptr, output->data(), &count);
  output->resize(count);
  return status;
}

bool matches(const Options &options,
             const std::vector<pdcm_entity_info_t> &items) {
  return !options.device.has_value() ||
         (items.size() == 1 && items.front().entity.pdcm_id == *options.device);
}

int version(const Options &options, std::ostream &output, std::ostream &error) {
  pdcm_version_info_t local = PDCM_VERSION_INFO_INIT;
  if (pdcm_version_get(nullptr, &local) != PDCM_STATUS_SUCCESS) {
    return 8;
  }
  Handle handle;
  pdcm_status_t status = open(options, &handle);
  pdcm_version_info_t full = PDCM_VERSION_INFO_INIT;
  if (status == PDCM_STATUS_SUCCESS) {
    status = pdcm_version_get(handle.get(), &full);
  }
  const bool connected = status == PDCM_STATUS_SUCCESS;
  if (options.format == Format::kJson) {
    output << "{\"schema_version\":1,\"command\":\"version\",\"status\":\""
           << (connected ? "SUCCESS" : "PARTIAL_RESULT")
           << "\",\"request_id\":0,\"catalog_generation\":"
           << (connected ? full.catalog_generation : 0) << ",\"core_state\":\""
           << (connected ? coreState(full.core_state) : "UNAVAILABLE")
           << "\",\"provider_state\":\""
           << (connected ? providerState(full.provider_state) : "UNAVAILABLE")
           << "\",\"items\":[{\"library_version\":\"" << local.library_version
           << "\",\"public_abi_major\":" << local.public_abi_major
           << ",\"public_abi_minor\":" << local.public_abi_minor
           << ",\"local_protocol_major\":" << local.local_protocol_major;
    if (connected) {
      output << ",\"daemon_version\":\"" << full.daemon_version << '"';
    }
    output << "}],\"errors\":[";
    if (!connected) {
      output << "{\"status\":\"" << statusName(status)
             << "\",\"reason\":\"DAEMON_UNAVAILABLE\"}";
    }
    output << "]}\n";
  } else {
    output << "libpdcm: " << local.library_version << "\n"
           << "public ABI: " << local.public_abi_major << '.'
           << local.public_abi_minor << "\n"
           << "local protocol: " << local.local_protocol_major << '.'
           << local.local_protocol_minor << "\n";
    if (connected) {
      output << "daemon: " << full.daemon_version << "\n"
             << "core: " << coreState(full.core_state) << "\n"
             << "provider: " << providerState(full.provider_state) << "\n"
             << "catalog generation: " << full.catalog_generation << "\n"
             << "scope: single-device\n";
    } else {
      output << "daemon: UNAVAILABLE\nprovider: UNAVAILABLE\n";
    }
  }
  if (!connected && options.verbose) {
    error << "pdcm-cli: daemon connection failed with " << statusName(status)
          << "\n";
  }
  return connected ? 0 : 1;
}

int discovery(const Options &options, std::ostream &output,
              std::ostream &error) {
  Handle handle;
  const pdcm_status_t opened = open(options, &handle);
  if (opened != PDCM_STATUS_SUCCESS) {
    if (options.verbose) {
      error << "pdcm-cli: open failed with " << statusName(opened) << "\n";
    }
    return exitCode(opened);
  }
  std::vector<pdcm_entity_info_t> items;
  const pdcm_status_t status = entities(handle.get(), &items);
  if (status != PDCM_STATUS_SUCCESS) {
    return exitCode(status);
  }
  if (!matches(options, items)) {
    return 3;
  }
  if (options.format == Format::kJson) {
    output << "{\"schema_version\":1,\"command\":\"discovery\","
              "\"status\":\"SUCCESS\",\"request_id\":0,"
              "\"catalog_generation\":"
           << (items.empty() ? 0 : items.front().entity.generation)
           << ",\"core_state\":\"READY\",\"provider_state\":\"READY\","
              "\"items\":[";
    if (!items.empty()) {
      const pdcm_entity_info_t &item = items.front();
      output << "{\"pdcm_id\":" << item.entity.pdcm_id
             << ",\"generation\":" << item.entity.generation
             << ",\"native_id\":\"" << item.native_id << "\",\"pci_bdf\":\""
             << item.pci_bdf << "\",\"pdrv_version\":\"" << item.pdrv_version
             << "\"}";
    }
    output << "],\"errors\":[]}\n";
  } else {
    output << "device count: " << items.size() << "\n";
    if (!items.empty()) {
      const pdcm_entity_info_t &item = items.front();
      output << "device " << item.entity.pdcm_id << "\n"
             << "  generation: " << item.entity.generation << "\n"
             << "  native ID: " << item.native_id << "\n"
             << "  PCI BDF: " << item.pci_bdf << "\n"
             << "  PDRV: " << item.pdrv_version << "\n";
    }
  }
  return 0;
}

int capabilityCommand(const Options &options, std::ostream &output,
                      std::ostream &error) {
  Handle handle;
  const pdcm_status_t opened = open(options, &handle);
  if (opened != PDCM_STATUS_SUCCESS) {
    if (options.verbose) {
      error << "pdcm-cli: open failed with " << statusName(opened) << "\n";
    }
    return exitCode(opened);
  }
  std::vector<pdcm_entity_info_t> devices;
  pdcm_status_t status = entities(handle.get(), &devices);
  if (status != PDCM_STATUS_SUCCESS) {
    return exitCode(status);
  }
  if (!matches(options, devices)) {
    return 3;
  }
  if (devices.empty()) {
    output << (options.format == Format::kJson
                   ? "{\"schema_version\":1,\"status\":\"SUCCESS\","
                     "\"items\":[],\"errors\":[]}\n"
                   : "device count: 0\n");
    return 0;
  }
  pdcm_capability_set_t capabilities = PDCM_CAPABILITY_SET_INIT;
  status = pdcm_capability_query(handle.get(), &devices.front().entity,
                                 &capabilities);
  if (status != PDCM_STATUS_BUFFER_TOO_SMALL && status != PDCM_STATUS_SUCCESS) {
    return exitCode(status);
  }
  std::vector<pdcm_capability_item_t> items(capabilities.item_count);
  for (pdcm_capability_item_t &item : items) {
    item.header.struct_size = sizeof(item);
    item.header.version = PDCM_STRUCT_VERSION_1;
  }
  capabilities.items = items.data();
  capabilities.item_capacity = items.size();
  status = pdcm_capability_query(handle.get(), &devices.front().entity,
                                 &capabilities);
  if (status != PDCM_STATUS_SUCCESS) {
    return exitCode(status);
  }
  for (const pdcm_capability_item_t &item : items) {
    if (options.command == Command::kDmon &&
        item.kind == PDCM_CAPABILITY_METRICS_CATALOG && !item.supported) {
      const char *reason =
          item.reason == PDCM_CAPABILITY_REASON_CATALOG_BLOCKED_EXTERNAL
              ? "CATALOG_BLOCKED_EXTERNAL"
              : "METRICS_UNSUPPORTED";
      if (options.format == Format::kJson) {
        output << "{\"schema_version\":1,\"command\":\"dmon\","
                  "\"status\":\"UNSUPPORTED\",\"request_id\":0,"
                  "\"catalog_generation\":"
               << capabilities.catalog_generation
               << ",\"core_state\":\"READY\",\"provider_state\":\"READY\","
                  "\"items\":[],\"errors\":[{\"status\":\"UNSUPPORTED\","
                  "\"reason\":\""
               << reason << "\"}]}\n";
      } else {
        output << "dmon: " << reason << "\n";
      }
      return 3;
    }
    if (options.command == Command::kHealth &&
        item.kind == PDCM_CAPABILITY_HEALTH && !item.supported) {
      output << (options.format == Format::kJson
                     ? "{\"schema_version\":1,\"command\":\"health\","
                       "\"status\":\"UNSUPPORTED\",\"items\":[],"
                       "\"errors\":[{\"reason\":\"HEARTBEAT_UNSUPPORTED\"}]}\n"
                     : "firmware heartbeat: UNSUPPORTED\n");
      return 3;
    }
  }
  if (options.command == Command::kHealth) {
    output << (options.format == Format::kJson
                   ? "{\"schema_version\":1,\"command\":\"health\","
                     "\"status\":\"PARTIAL_RESULT\",\"items\":[{"
                     "\"subsystem\":\"firmware_heartbeat\","
                     "\"state\":\"UNKNOWN\","
                     "\"limitation\":\"PUBLIC_HEALTH_QUERY_PENDING\"}],"
                     "\"errors\":[]}\n"
                   : "firmware heartbeat: UNKNOWN "
                     "(PUBLIC_HEALTH_QUERY_PENDING)\n");
    return 1;
  }
  return 3;
}

} // namespace

int run(const std::vector<std::string> &arguments, std::ostream &output,
        std::ostream &error) {
  const ParseResult parsed = parse(arguments);
  if (!parsed.error.empty()) {
    error << "pdcm-cli: " << parsed.error << "\n";
    return 2;
  }
  if (parsed.options.command == Command::kHelp) {
    help(std::nullopt, output);
    return 0;
  }
  if (parsed.options.command_help) {
    help(parsed.options.command, output);
    return 0;
  }
  switch (parsed.options.command) {
  case Command::kVersion:
    return version(parsed.options, output, error);
  case Command::kDiscovery:
    return discovery(parsed.options, output, error);
  case Command::kDmon:
  case Command::kHealth:
    return capabilityCommand(parsed.options, output, error);
  case Command::kHelp:
    return 0;
  }
  return 8;
}

} // namespace pdcm::cli
