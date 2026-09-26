#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli/cli.hpp"

namespace pdcm::cli {
namespace {

struct Result {
  int exit_code{0};
  std::string output;
  std::string error;
};

Result invoke(std::vector<std::string> arguments) {
  std::ostringstream output;
  std::ostringstream error;
  const int exit_code = run(arguments, output, error);
  return {exit_code, output.str(), error.str()};
}

TEST(CliTest, HelpIsLocalAndContainsOnlyP0Commands) {
  const Result result = invoke({"--help"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.output.find("version"), std::string::npos);
  EXPECT_NE(result.output.find("discovery"), std::string::npos);
  EXPECT_NE(result.output.find("dmon"), std::string::npos);
  EXPECT_NE(result.output.find("health"), std::string::npos);
  EXPECT_EQ(result.output.find("diag"), std::string::npos);
  EXPECT_EQ(result.output.find("reset"), std::string::npos);
}

TEST(CliTest, CommandHelpDocumentsBlockedCatalogAndSingleDevice) {
  const Result result = invoke({"dmon", "--help"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.output.find("BLOCKED_EXTERNAL"), std::string::npos);
  EXPECT_NE(result.output.find("one Device"), std::string::npos);
}

TEST(CliTest, RejectsUnknownCommandsAndInvalidGlobalOptions) {
  EXPECT_EQ(invoke({"diag"}).exit_code, 2);
  EXPECT_EQ(invoke({"version", "--endpoint", "relative.sock"}).exit_code, 2);
  EXPECT_EQ(invoke({"version", "--timeout", "0ms"}).exit_code, 2);
  EXPECT_EQ(invoke({"version", "--format", "yaml"}).exit_code, 2);
  EXPECT_EQ(invoke({"health", "--metric", "fake"}).exit_code, 2);
}

TEST(CliTest, VersionJsonKeepsLocalDataWhenDaemonIsAbsent) {
  const Result result = invoke({"version", "--format", "json", "--endpoint",
                                "/tmp/pdcm-cli-definitely-missing.sock"});
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.output.find("\"command\":\"version\""), std::string::npos);
  EXPECT_NE(result.output.find("\"status\":\"PARTIAL_RESULT\""),
            std::string::npos);
  EXPECT_NE(result.output.find("\"library_version\":\"0.1.0\""),
            std::string::npos);
  EXPECT_NE(result.output.find("\"reason\":\"DAEMON_UNAVAILABLE\""),
            std::string::npos);
}

TEST(CliTest, EmbeddedDiscoveryReportsProviderUnavailable) {
  const Result result = invoke({"discovery", "--mode", "embedded"});
  EXPECT_EQ(result.exit_code, 5);
}

} // namespace
} // namespace pdcm::cli
