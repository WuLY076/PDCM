#ifndef PDCM_TESTKIT_MANUAL_CLOCK_HPP_
#define PDCM_TESTKIT_MANUAL_CLOCK_HPP_

#include <atomic>
#include <cstdint>
#include <stdexcept>

#include "common/clock.hpp"

namespace pdcm::testkit {

class ManualClock final : public Clock {
public:
  ManualClock(std::int64_t monotonic_ns, std::int64_t wall_ns) noexcept
      : monotonic_ns_(monotonic_ns), wall_ns_(wall_ns) {}

  [[nodiscard]] MonotonicTime monotonicNow() const noexcept override {
    return MonotonicTime(Nanoseconds(monotonic_ns_.load()));
  }

  [[nodiscard]] std::int64_t wallTimeNanoseconds() const noexcept override {
    return wall_ns_.load();
  }

  void advance(Nanoseconds delta) {
    if (delta.count() < 0) {
      throw std::invalid_argument("manual clock cannot move backwards");
    }
    monotonic_ns_.fetch_add(delta.count());
    wall_ns_.fetch_add(delta.count());
  }

private:
  std::atomic<std::int64_t> monotonic_ns_;
  std::atomic<std::int64_t> wall_ns_;
};

} // namespace pdcm::testkit

#endif // PDCM_TESTKIT_MANUAL_CLOCK_HPP_
