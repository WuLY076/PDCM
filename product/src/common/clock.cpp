#include "common/clock.hpp"

namespace pdcm {

MonotonicTime SystemClock::monotonicNow() const noexcept {
  return std::chrono::time_point_cast<Nanoseconds>(
      std::chrono::steady_clock::now());
}

std::int64_t SystemClock::wallTimeNanoseconds() const noexcept {
  return std::chrono::duration_cast<Nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace pdcm
