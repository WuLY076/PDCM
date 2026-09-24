#ifndef PDCM_DATA_SUBSCRIPTION_MANAGER_HPP_
#define PDCM_DATA_SUBSCRIPTION_MANAGER_HPP_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "common/status.hpp"
#include "data/data_manager.hpp"
#include "data/event.hpp"

namespace pdcm {

struct SubscriptionFilter {
  std::vector<EventType> event_types;
  std::optional<EntityRef> entity;
  std::vector<MetricId> metrics;
  std::optional<std::uint32_t> health_subsystem;
  EventSeverity minimum_severity{EventSeverity::kInfo};
  bool follow_rediscovery{false};
};

struct SubscriptionLimits {
  std::size_t max_events{64};
  std::size_t max_bytes{256U * 1024U};

  [[nodiscard]] Status validate() const;
};

struct SubscribeResult {
  Status status;
  std::uint64_t subscription_id{0};
};

using EventCallback = std::function<void(const PdcmEvent &)>;

class SubscriptionManager {
public:
  explicit SubscriptionManager(DataManager &data_manager,
                               std::size_t max_subscriptions = 1024);

  SubscriptionManager(const SubscriptionManager &) = delete;
  SubscriptionManager &operator=(const SubscriptionManager &) = delete;

  [[nodiscard]] SubscribeResult subscribe(SubscriptionFilter filter,
                                          SubscriptionLimits limits = {});
  [[nodiscard]] Status dispatch();
  [[nodiscard]] Status deliverNext(std::uint64_t subscription_id,
                                   const EventCallback &callback);
  [[nodiscard]] Status close(std::uint64_t subscription_id);
  [[nodiscard]] Status drain(std::int64_t occurrence_time_ns);

  [[nodiscard]] std::size_t pendingCount(std::uint64_t subscription_id) const;
  [[nodiscard]] std::size_t pendingBytes(std::uint64_t subscription_id) const;
  [[nodiscard]] bool isClosed(std::uint64_t subscription_id) const;

private:
  struct Subscription {
    SubscriptionFilter filter;
    SubscriptionLimits limits;
    mutable std::mutex mutex;
    std::condition_variable idle;
    std::deque<std::shared_ptr<const PdcmEvent>> pending;
    std::size_t pending_bytes{0};
    std::size_t active_deliveries{0};
    bool closing{false};
    bool closed{false};
  };

  [[nodiscard]] static Status validateFilter(SubscriptionFilter &filter);
  [[nodiscard]] static bool matches(const SubscriptionFilter &filter,
                                    const PdcmEvent &event);
  [[nodiscard]] static bool sameMetricKey(const PdcmEvent &lhs,
                                          const PdcmEvent &rhs);
  [[nodiscard]] static std::shared_ptr<const PdcmEvent>
  lossMarker(const PdcmEvent &dropped,
             const std::shared_ptr<const PdcmEvent> &existing);
  static void enqueue(const std::shared_ptr<Subscription> &subscription,
                      const std::shared_ptr<const PdcmEvent> &event);

  DataManager &data_manager_;
  const std::size_t max_subscriptions_;
  mutable std::mutex registry_mutex_;
  mutable std::mutex dispatch_mutex_;
  std::map<std::uint64_t, std::shared_ptr<Subscription>> subscriptions_;
  std::map<std::uint64_t, std::weak_ptr<Subscription>> known_subscriptions_;
  std::uint64_t next_subscription_id_{1};
  std::uint64_t last_seen_sequence_{0};
  bool draining_{false};
};

} // namespace pdcm

#endif // PDCM_DATA_SUBSCRIPTION_MANAGER_HPP_
