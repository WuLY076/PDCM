#ifndef PDCM_COMMON_CLOCK_HPP_
#define PDCM_COMMON_CLOCK_HPP_

#include <chrono>
#include <cstdint>

namespace pdcm {

using Nanoseconds = std::chrono::nanoseconds;
using MonotonicTime =
    std::chrono::time_point<std::chrono::steady_clock, Nanoseconds>;

class Clock {
public:
  virtual ~Clock() = default;

  [[nodiscard]] virtual MonotonicTime monotonicNow() const noexcept = 0;
  [[nodiscard]] virtual std::int64_t wallTimeNanoseconds() const noexcept = 0;
};

class SystemClock final : public Clock {
public:
  [[nodiscard]] MonotonicTime monotonicNow() const noexcept override;
  [[nodiscard]] std::int64_t wallTimeNanoseconds() const noexcept override;
};

} // namespace pdcm

#endif // PDCM_COMMON_CLOCK_HPP_
