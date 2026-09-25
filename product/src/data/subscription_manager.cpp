#include "data/subscription_manager.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace pdcm {
namespace {

bool severityAtLeast(const EventSeverity actual,
                     const EventSeverity minimum) noexcept {
  return static_cast<std::uint8_t>(actual) >=
         static_cast<std::uint8_t>(minimum);
}

bool droppable(const PdcmEvent &event) noexcept {
  return !isNonDroppable(event.type);
}

} // namespace

Status SubscriptionLimits::validate() const {
  if (max_events == 0 || max_bytes == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "subscription limits must be non-zero");
  }
  return Status::success();
}

SubscriptionManager::SubscriptionManager(DataManager &data_manager,
                                         const std::size_t max_subscriptions)
    : data_manager_(data_manager), max_subscriptions_(max_subscriptions) {
  if (max_subscriptions_ == 0) {
    throw std::invalid_argument("subscription limit must be non-zero");
  }
}

Status SubscriptionManager::validateFilter(SubscriptionFilter &filter) {
  if (filter.event_types.empty() ||
      (filter.health_subsystem.has_value() &&
       *filter.health_subsystem != kFirmwareHeartbeatHealthId)) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "subscription filter is invalid");
  }
  std::sort(filter.event_types.begin(), filter.event_types.end());
  if (std::adjacent_find(filter.event_types.begin(),
                         filter.event_types.end()) !=
      filter.event_types.end()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "subscription event types contain duplicates");
  }
  std::sort(filter.metrics.begin(), filter.metrics.end(),
            [](const MetricId lhs, const MetricId rhs) {
              return lhs.value < rhs.value;
            });
  if (std::adjacent_find(filter.metrics.begin(), filter.metrics.end()) !=
      filter.metrics.end()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "subscription metrics contain duplicates");
  }
  return Status::success();
}

SubscribeResult
SubscriptionManager::subscribe(SubscriptionFilter filter,
                               const SubscriptionLimits limits) {
  SubscribeResult result;
  result.status = validateFilter(filter);
  if (!result.status.ok()) {
    return result;
  }
  result.status = limits.validate();
  if (!result.status.ok()) {
    return result;
  }

  std::lock_guard<std::mutex> lock(registry_mutex_);
  if (draining_) {
    result.status =
        Status(PDCM_STATUS_UNAVAILABLE, "subscription manager is draining");
    return result;
  }
  if (subscriptions_.size() >= max_subscriptions_ ||
      next_subscription_id_ == std::numeric_limits<std::uint64_t>::max()) {
    result.status = Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                           "subscription registry limit reached");
    return result;
  }

  auto subscription = std::make_shared<Subscription>();
  subscription->filter = std::move(filter);
  subscription->limits = limits;
  result.subscription_id = next_subscription_id_++;
  subscriptions_.emplace(result.subscription_id, subscription);
  known_subscriptions_[result.subscription_id] = subscription;
  result.status = Status::success();
  return result;
}

bool SubscriptionManager::matches(const SubscriptionFilter &filter,
                                  const PdcmEvent &event) {
  if (!std::binary_search(filter.event_types.begin(), filter.event_types.end(),
                          event.type) ||
      !severityAtLeast(event.severity, filter.minimum_severity)) {
    return false;
  }
  if (filter.entity.has_value()) {
    if (!event.entity.has_value()) {
      return false;
    }
    const bool exact = *filter.entity == *event.entity;
    const bool rebound = filter.follow_rediscovery &&
                         filter.entity->kind == event.entity->kind &&
                         filter.entity->id == event.entity->id;
    if (!exact && !rebound) {
      return false;
    }
  }
  if (!filter.metrics.empty()) {
    if (!event.metric.has_value() ||
        !std::binary_search(filter.metrics.begin(), filter.metrics.end(),
                            *event.metric,
                            [](const MetricId lhs, const MetricId rhs) {
                              return lhs.value < rhs.value;
                            })) {
      return false;
    }
  }
  if (filter.health_subsystem.has_value() &&
      event.health_subsystem != filter.health_subsystem) {
    return false;
  }
  return true;
}

bool SubscriptionManager::sameMetricKey(const PdcmEvent &lhs,
                                        const PdcmEvent &rhs) {
  return lhs.type == EventType::kMetricUpdate &&
         rhs.type == EventType::kMetricUpdate && lhs.entity == rhs.entity &&
         lhs.metric == rhs.metric;
}

std::shared_ptr<const PdcmEvent> SubscriptionManager::lossMarker(
    const PdcmEvent &dropped,
    const std::shared_ptr<const PdcmEvent> &existing) {
  auto marker = std::make_shared<PdcmEvent>();
  marker->boot_epoch = dropped.boot_epoch;
  marker->sequence = dropped.sequence;
  marker->type = EventType::kSubscriptionLossMarker;
  marker->severity = EventSeverity::kWarning;
  marker->occurrence_time_ns = dropped.occurrence_time_ns;
  marker->catalog_generation = dropped.catalog_generation;
  SubscriptionLossPayload payload{dropped.sequence, dropped.sequence, 1};
  if (existing != nullptr) {
    marker->sequence = existing->sequence;
    marker->occurrence_time_ns = existing->occurrence_time_ns;
    payload = std::get<SubscriptionLossPayload>(existing->payload);
    payload.first_dropped_sequence =
        std::min(payload.first_dropped_sequence, dropped.sequence);
    payload.last_dropped_sequence =
        std::max(payload.last_dropped_sequence, dropped.sequence);
    ++payload.dropped_count;
  }
  marker->payload = payload;
  return marker;
}

void SubscriptionManager::enqueue(
    const std::shared_ptr<Subscription> &subscription,
    const std::shared_ptr<const PdcmEvent> &event) {
  std::lock_guard<std::mutex> lock(subscription->mutex);
  if (subscription->closing || subscription->closed) {
    return;
  }

  const std::size_t incoming_bytes = EventStore::eventBytes(*event);
  if (incoming_bytes > subscription->limits.max_bytes) {
    subscription->pending.clear();
    subscription->pending_bytes = 0;
    subscription->closed = true;
    return;
  }

  std::shared_ptr<const PdcmEvent> marker;
  for (auto iterator = subscription->pending.begin();
       iterator != subscription->pending.end(); ++iterator) {
    if ((*iterator)->type == EventType::kSubscriptionLossMarker) {
      marker = *iterator;
      break;
    }
  }

  std::vector<std::shared_ptr<const PdcmEvent>> dropped;
  const std::size_t marker_reserve =
      marker == nullptr ? EventStore::eventBytes(*lossMarker(*event, nullptr))
                        : 0;
  const auto exceeds_limits = [&] {
    const bool needs_marker = marker == nullptr && !dropped.empty();
    const std::size_t reserved = needs_marker ? marker_reserve : 0;
    const bool event_limit =
        subscription->pending.size() + 1U + (needs_marker ? 1U : 0U) >
        subscription->limits.max_events;
    const bool byte_limit = incoming_bytes > subscription->limits.max_bytes -
                                                 subscription->pending_bytes ||
                            reserved > subscription->limits.max_bytes -
                                           subscription->pending_bytes -
                                           incoming_bytes;
    return event_limit || byte_limit;
  };
  while (exceeds_limits()) {
    auto candidate = subscription->pending.end();
    if (event->type == EventType::kMetricUpdate) {
      candidate = std::find_if(
          subscription->pending.begin(), subscription->pending.end(),
          [&event](const std::shared_ptr<const PdcmEvent> &queued) {
            return sameMetricKey(*queued, *event);
          });
    } else {
      candidate = std::find_if(
          subscription->pending.begin(), subscription->pending.end(),
          [](const std::shared_ptr<const PdcmEvent> &queued) {
            return droppable(*queued);
          });
    }
    if (candidate == subscription->pending.end()) {
      subscription->pending.clear();
      subscription->pending_bytes = 0;
      subscription->closed = true;
      return;
    }
    subscription->pending_bytes -= EventStore::eventBytes(**candidate);
    dropped.push_back(*candidate);
    subscription->pending.erase(candidate);
  }

  for (const std::shared_ptr<const PdcmEvent> &dropped_event : dropped) {
    marker = lossMarker(*dropped_event, marker);
  }
  if (!dropped.empty()) {
    const auto marker_position = std::find_if(
        subscription->pending.begin(), subscription->pending.end(),
        [](const std::shared_ptr<const PdcmEvent> &queued) {
          return queued->type == EventType::kSubscriptionLossMarker;
        });
    if (marker_position != subscription->pending.end()) {
      subscription->pending_bytes -= EventStore::eventBytes(**marker_position);
      *marker_position = marker;
      subscription->pending_bytes += EventStore::eventBytes(*marker);
    } else {
      subscription->pending.push_back(marker);
      subscription->pending_bytes += EventStore::eventBytes(*marker);
    }
  }

  if (subscription->pending.size() >= subscription->limits.max_events ||
      subscription->pending_bytes >
          subscription->limits.max_bytes - incoming_bytes) {
    subscription->pending.clear();
    subscription->pending_bytes = 0;
    subscription->closed = true;
    return;
  }
  subscription->pending.push_back(event);
  subscription->pending_bytes += incoming_bytes;
}

Status SubscriptionManager::dispatch() {
  std::lock_guard<std::mutex> dispatch_lock(dispatch_mutex_);
  const EventSnapshot snapshot = data_manager_.eventsSince(last_seen_sequence_);
  if (snapshot.events.empty()) {
    return Status::success();
  }

  std::vector<std::shared_ptr<Subscription>> subscriptions;
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    subscriptions.reserve(subscriptions_.size());
    for (const auto &entry : subscriptions_) {
      subscriptions.push_back(entry.second);
    }
    last_seen_sequence_ = snapshot.latest_sequence;
  }

  for (const std::shared_ptr<const PdcmEvent> &event : snapshot.events) {
    for (const std::shared_ptr<Subscription> &subscription : subscriptions) {
      if (matches(subscription->filter, *event)) {
        enqueue(subscription, event);
      }
    }
  }
  return Status::success();
}

Status SubscriptionManager::deliverNext(const std::uint64_t subscription_id,
                                        const EventCallback &callback) {
  if (!callback) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT, "event callback is empty");
  }

  std::shared_ptr<Subscription> subscription;
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    const auto found = subscriptions_.find(subscription_id);
    if (found == subscriptions_.end()) {
      return Status(PDCM_STATUS_NOT_FOUND, "subscription is not active");
    }
    subscription = found->second;
  }

  std::shared_ptr<const PdcmEvent> event;
  {
    std::lock_guard<std::mutex> lock(subscription->mutex);
    if (subscription->closing || subscription->closed) {
      return Status(PDCM_STATUS_UNAVAILABLE, "subscription is closed");
    }
    if (subscription->pending.empty()) {
      return Status(PDCM_STATUS_NOT_FOUND, "subscription queue is empty");
    }
    event = subscription->pending.front();
    subscription->pending.pop_front();
    subscription->pending_bytes -= EventStore::eventBytes(*event);
    ++subscription->active_deliveries;
  }

  Status result = Status::success();
  try {
    callback(*event);
  } catch (...) {
    result = Status(PDCM_STATUS_INTERNAL, "event callback threw an exception");
  }

  {
    std::lock_guard<std::mutex> lock(subscription->mutex);
    --subscription->active_deliveries;
    subscription->idle.notify_all();
  }
  return result;
}

Status SubscriptionManager::close(const std::uint64_t subscription_id) {
  std::shared_ptr<Subscription> subscription;
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    const auto found = subscriptions_.find(subscription_id);
    if (found == subscriptions_.end()) {
      return Status(PDCM_STATUS_NOT_FOUND, "subscription is not active");
    }
    subscription = found->second;
    subscriptions_.erase(found);
  }

  std::unique_lock<std::mutex> lock(subscription->mutex);
  subscription->closing = true;
  subscription->pending.clear();
  subscription->pending_bytes = 0;
  subscription->idle.wait(
      lock, [&subscription] { return subscription->active_deliveries == 0; });
  subscription->closed = true;
  return Status::success();
}

Status SubscriptionManager::drain(const std::int64_t occurrence_time_ns) {
  if (occurrence_time_ns < 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT, "drain time is invalid");
  }
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    draining_ = true;
  }
  EventDraft draft;
  draft.type = EventType::kDaemonDraining;
  draft.severity = EventSeverity::kWarning;
  draft.occurrence_time_ns = occurrence_time_ns;
  draft.catalog_generation = data_manager_.catalogGeneration();
  const EventPublishResult published = data_manager_.publishEvent(draft);
  if (!published.status.ok()) {
    return published.status;
  }
  return dispatch();
}

std::size_t
SubscriptionManager::pendingCount(const std::uint64_t subscription_id) const {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  const auto known = known_subscriptions_.find(subscription_id);
  if (known == known_subscriptions_.end()) {
    return 0;
  }
  const std::shared_ptr<Subscription> subscription = known->second.lock();
  if (subscription == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> queue_lock(subscription->mutex);
  return subscription->pending.size();
}

std::size_t
SubscriptionManager::pendingBytes(const std::uint64_t subscription_id) const {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  const auto known = known_subscriptions_.find(subscription_id);
  if (known == known_subscriptions_.end()) {
    return 0;
  }
  const std::shared_ptr<Subscription> subscription = known->second.lock();
  if (subscription == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> queue_lock(subscription->mutex);
  return subscription->pending_bytes;
}

bool SubscriptionManager::isClosed(const std::uint64_t subscription_id) const {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  const auto known = known_subscriptions_.find(subscription_id);
  if (known == known_subscriptions_.end()) {
    return true;
  }
  const std::shared_ptr<Subscription> subscription = known->second.lock();
  if (subscription == nullptr) {
    return true;
  }
  std::lock_guard<std::mutex> queue_lock(subscription->mutex);
  return subscription->closed;
}

} // namespace pdcm
