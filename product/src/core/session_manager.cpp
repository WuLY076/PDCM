#include "core/session_manager.hpp"

#include <limits>

namespace pdcm {

SessionManager::SessionManager(ResourceLimits limits) : limits_(limits) {}

SessionCreateResult SessionManager::beginHandshake(const PeerIdentity peer) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (draining_) {
    return {Status(PDCM_STATUS_UNAVAILABLE, "session manager is draining"), 0};
  }
  if (sessions_.size() >= limits_.max_sessions) {
    return {
        Status(PDCM_STATUS_RESOURCE_EXHAUSTED, "session capacity exhausted"),
        0};
  }
  if (next_session_id_ == 0) {
    return {Status(PDCM_STATUS_INTERNAL, "session identifier exhausted"), 0};
  }

  const SessionId id = next_session_id_;
  if (next_session_id_ == std::numeric_limits<SessionId>::max()) {
    next_session_id_ = 0;
  } else {
    ++next_session_id_;
  }

  SessionRecord record;
  record.snapshot.id = id;
  record.snapshot.state = SessionState::kHandshaking;
  record.snapshot.peer = peer;
  sessions_.emplace(id, record);
  return {Status::success(), id};
}

Status SessionManager::activate(const SessionId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iterator = sessions_.find(id);
  if (iterator == sessions_.end()) {
    return Status(PDCM_STATUS_NOT_FOUND, "session does not exist");
  }
  if (draining_ ||
      iterator->second.snapshot.state != SessionState::kHandshaking) {
    return Status(PDCM_STATUS_UNAVAILABLE,
                  "session cannot transition to active");
  }
  iterator->second.snapshot.state = SessionState::kActive;
  return Status::success();
}

Status SessionManager::beginDraining() {
  std::lock_guard<std::mutex> lock(mutex_);
  draining_ = true;
  for (auto &entry : sessions_) {
    SessionSnapshot &value = entry.second.snapshot;
    if (value.state == SessionState::kActive) {
      value.state = SessionState::kDraining;
    } else if (value.state == SessionState::kHandshaking) {
      value.state = SessionState::kClosing;
    }
  }
  return Status::success();
}

Status SessionManager::close(const SessionId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iterator = sessions_.find(id);
  if (iterator == sessions_.end()) {
    return Status(PDCM_STATUS_NOT_FOUND, "session does not exist");
  }
  iterator->second.snapshot.state = SessionState::kClosing;
  sessions_.erase(iterator);
  return Status::success();
}

void SessionManager::closeAll() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  draining_ = true;
  sessions_.clear();
}

Status SessionManager::reserveRequest(const SessionId id) {
  return reserve(id, CounterKind::kRequest);
}

Status SessionManager::releaseRequest(const SessionId id) {
  return release(id, CounterKind::kRequest);
}

Status SessionManager::reserveWatch(const SessionId id) {
  return reserve(id, CounterKind::kWatch);
}

Status SessionManager::releaseWatch(const SessionId id) {
  return release(id, CounterKind::kWatch);
}

Status SessionManager::reserveSubscription(const SessionId id) {
  return reserve(id, CounterKind::kSubscription);
}

Status SessionManager::releaseSubscription(const SessionId id) {
  return release(id, CounterKind::kSubscription);
}

std::optional<SessionSnapshot>
SessionManager::snapshot(const SessionId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iterator = sessions_.find(id);
  if (iterator == sessions_.end()) {
    return std::nullopt;
  }
  return iterator->second.snapshot;
}

std::size_t SessionManager::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

Status SessionManager::reserve(const SessionId id, const CounterKind kind) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iterator = sessions_.find(id);
  if (iterator == sessions_.end()) {
    return Status(PDCM_STATUS_NOT_FOUND, "session does not exist");
  }
  if (iterator->second.snapshot.state != SessionState::kActive) {
    return Status(PDCM_STATUS_UNAVAILABLE,
                  "session is not accepting new resources");
  }

  std::size_t &value = counter(iterator->second.snapshot.counters, kind);
  if (value >= limit(kind)) {
    return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                  "session resource quota exhausted");
  }
  ++value;
  return Status::success();
}

Status SessionManager::release(const SessionId id, const CounterKind kind) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iterator = sessions_.find(id);
  if (iterator == sessions_.end()) {
    return Status(PDCM_STATUS_NOT_FOUND, "session does not exist");
  }

  std::size_t &value = counter(iterator->second.snapshot.counters, kind);
  if (value == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "session resource counter is already zero");
  }
  --value;
  return Status::success();
}

std::size_t SessionManager::limit(const CounterKind kind) const {
  switch (kind) {
  case CounterKind::kRequest:
    return limits_.max_outstanding_requests_per_session;
  case CounterKind::kWatch:
    return limits_.max_watches_per_session;
  case CounterKind::kSubscription:
    return limits_.max_subscriptions_per_session;
  }
  return 0;
}

std::size_t &SessionManager::counter(SessionCounters &counters,
                                     const CounterKind kind) {
  switch (kind) {
  case CounterKind::kRequest:
    return counters.outstanding_requests;
  case CounterKind::kWatch:
    return counters.watches;
  case CounterKind::kSubscription:
    return counters.subscriptions;
  }
  return counters.outstanding_requests;
}

} // namespace pdcm
