#ifndef PDCM_CLI_CLI_HPP_
#define PDCM_CLI_CLI_HPP_

#include <iosfwd>
#include <string>
#include <vector>

namespace pdcm::cli {

int run(const std::vector<std::string> &arguments, std::ostream &output,
        std::ostream &error);

} // namespace pdcm::cli

#endif // PDCM_CLI_CLI_HPP_
