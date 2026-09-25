#ifndef PDCM_COMMON_STATUS_HPP_
#define PDCM_COMMON_STATUS_HPP_

#include <string>
#include <utility>

#include "pdcm/status.h"

namespace pdcm {

class Status {
public:
  Status() noexcept = default;
  explicit Status(pdcm_status_t code) noexcept : code_(code) {}
  Status(pdcm_status_t code, std::string message)
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] static Status success() noexcept { return Status{}; }

  [[nodiscard]] bool ok() const noexcept {
    return code_ == PDCM_STATUS_SUCCESS;
  }

  [[nodiscard]] pdcm_status_t code() const noexcept { return code_; }
  [[nodiscard]] const std::string &message() const noexcept { return message_; }

private:
  pdcm_status_t code_{PDCM_STATUS_SUCCESS};
  std::string message_;
};

[[nodiscard]] const char *statusName(pdcm_status_t status) noexcept;

} // namespace pdcm

#endif // PDCM_COMMON_STATUS_HPP_
