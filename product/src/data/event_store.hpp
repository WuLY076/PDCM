#ifndef PDCM_DATA_EVENT_STORE_HPP_
#define PDCM_DATA_EVENT_STORE_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "common/status.hpp"
#include "data/event.hpp"

namespace pdcm {

struct EventStoreLimits {
  std::size_t max_events{4096};
  std::size_t max_bytes{4U * 1024U * 1024U};

  [[nodiscard]] Status validate() const;
};

struct EventPublishResult {
  Status status;
  std::shared_ptr<const PdcmEvent> event;
};

struct EventSnapshot {
  std::uint64_t first_available_sequence{0};
  std::uint64_t latest_sequence{0};
  std::vector<std::shared_ptr<const PdcmEvent>> events;
};

class EventStore {
public:
  explicit EventStore(EventStoreLimits limits = {},
                      std::uint64_t boot_epoch = 1);

  [[nodiscard]] EventPublishResult publish(const EventDraft &draft);
  [[nodiscard]] EventSnapshot since(std::uint64_t sequence) const;
  [[nodiscard]] std::size_t eventCount() const;
  [[nodiscard]] std::size_t storedBytes() const;

  [[nodiscard]] static std::size_t eventBytes(const PdcmEvent &event);

private:
  EventStoreLimits limits_;
  std::uint64_t boot_epoch_;
  mutable std::mutex mutex_;
  std::uint64_t next_sequence_{1};
  std::size_t stored_bytes_{0};
  std::deque<std::shared_ptr<const PdcmEvent>> events_;
};

} // namespace pdcm

#endif // PDCM_DATA_EVENT_STORE_HPP_
