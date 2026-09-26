#include "cli/cli.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(const int argc, char **const argv) {
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  return pdcm::cli::run(arguments, std::cout, std::cerr);
}
