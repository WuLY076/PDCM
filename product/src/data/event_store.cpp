#include "data/event_store.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace pdcm {

bool isNonDroppable(const EventType type) noexcept {
  return type == EventType::kSubscriptionLossMarker ||
         type == EventType::kDaemonDraining;
}

bool isStateChange(const EventType type) noexcept {
  return type == EventType::kCatalogChanged ||
         type == EventType::kEntityChanged ||
         type == EventType::kCapabilityChanged ||
         type == EventType::kFirmwareHeartbeatHealthChanged ||
         type == EventType::kProviderStateChanged;
}

Status EventStoreLimits::validate() const {
  if (max_events == 0 || max_bytes == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "event store limits must be non-zero");
  }
  return Status::success();
}

EventStore::EventStore(EventStoreLimits limits, const std::uint64_t boot_epoch)
    : limits_(limits), boot_epoch_(boot_epoch) {
  const Status status = limits_.validate();
  if (!status.ok() || boot_epoch_ == 0) {
    throw std::invalid_argument("event store configuration is invalid");
  }
}

std::size_t EventStore::eventBytes(const PdcmEvent &event) {
  std::size_t bytes = sizeof(PdcmEvent);
  bytes += std::visit(
      [](const auto &payload) -> std::size_t {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, std::string>) {
          return payload.size();
        } else if constexpr (std::is_same_v<Payload, HealthChangePayload>) {
          std::size_t payload_bytes =
              sizeof(Payload) + payload.evidence.size() * sizeof(EvidenceRef);
          for (const EvidenceRef &evidence : payload.evidence) {
            payload_bytes += evidence.source.provider.size();
            payload_bytes += evidence.source.native_source.size();
          }
          return payload_bytes;
        } else {
          return sizeof(Payload);
        }
      },
      event.payload);
  return bytes;
}

EventPublishResult EventStore::publish(const EventDraft &draft) {
  EventPublishResult result;
  if (draft.occurrence_time_ns < 0 || draft.catalog_generation == 0 ||
      (draft.type == EventType::kMetricUpdate &&
       (!draft.entity.has_value() || !draft.metric.has_value())) ||
      (draft.type == EventType::kFirmwareHeartbeatHealthChanged &&
       draft.health_subsystem != kFirmwareHeartbeatHealthId)) {
    result.status =
        Status(PDCM_STATUS_INVALID_ARGUMENT, "event draft is malformed");
    return result;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    result.status = Status(PDCM_STATUS_INTERNAL, "event sequence overflow");
    return result;
  }

  auto event = std::make_shared<PdcmEvent>();
  event->boot_epoch = boot_epoch_;
  event->sequence = next_sequence_++;
  event->type = draft.type;
  event->severity = draft.severity;
  event->entity = draft.entity;
  event->metric = draft.metric;
  event->health_subsystem = draft.health_subsystem;
  event->occurrence_time_ns = draft.occurrence_time_ns;
  event->catalog_generation = draft.catalog_generation;
  event->payload = draft.payload;

  const std::size_t bytes = eventBytes(*event);
  if (bytes > limits_.max_bytes) {
    result.status =
        Status(PDCM_STATUS_RESOURCE_EXHAUSTED, "event exceeds byte limit");
    return result;
  }

  events_.push_back(event);
  stored_bytes_ += bytes;
  while (events_.size() > limits_.max_events ||
         stored_bytes_ > limits_.max_bytes) {
    stored_bytes_ -= eventBytes(*events_.front());
    events_.pop_front();
  }

  result.status = Status::success();
  result.event = std::move(event);
  return result;
}

EventSnapshot EventStore::since(const std::uint64_t sequence) const {
  std::lock_guard<std::mutex> lock(mutex_);
  EventSnapshot snapshot;
  snapshot.latest_sequence = next_sequence_ - 1;
  snapshot.first_available_sequence =
      events_.empty() ? next_sequence_ : events_.front()->sequence;
  const auto begin =
      std::upper_bound(events_.begin(), events_.end(), sequence,
                       [](const std::uint64_t value,
                          const std::shared_ptr<const PdcmEvent> &event) {
                         return value < event->sequence;
                       });
  snapshot.events.assign(begin, events_.end());
  return snapshot;
}

std::size_t EventStore::eventCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_.size();
}

std::size_t EventStore::storedBytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stored_bytes_;
}

} // namespace pdcm
