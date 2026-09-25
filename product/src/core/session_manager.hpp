#ifndef PDCM_CORE_SESSION_MANAGER_HPP_
#define PDCM_CORE_SESSION_MANAGER_HPP_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "common/status.hpp"
#include "core/runtime_config.hpp"

namespace pdcm {

using SessionId = std::uint64_t;

enum class SessionState : std::uint8_t {
  kHandshaking,
  kActive,
  kDraining,
  kClosing,
};

struct PeerIdentity {
  std::uint32_t user_id{0};
  std::uint32_t group_id{0};
  std::uint64_t process_id{0};
};

struct SessionCounters {
  std::size_t outstanding_requests{0};
  std::size_t watches{0};
  std::size_t subscriptions{0};
};

struct SessionSnapshot {
  SessionId id{0};
  SessionState state{SessionState::kHandshaking};
  PeerIdentity peer;
  SessionCounters counters;
};

struct SessionCreateResult {
  Status status;
  SessionId id{0};
};

class SessionManager {
public:
  explicit SessionManager(ResourceLimits limits);

  SessionCreateResult beginHandshake(PeerIdentity peer);
  Status activate(SessionId id);
  Status beginDraining();
  Status close(SessionId id);
  void closeAll() noexcept;

  Status reserveRequest(SessionId id);
  Status releaseRequest(SessionId id);
  Status reserveWatch(SessionId id);
  Status releaseWatch(SessionId id);
  Status reserveSubscription(SessionId id);
  Status releaseSubscription(SessionId id);

  [[nodiscard]] std::optional<SessionSnapshot> snapshot(SessionId id) const;
  [[nodiscard]] std::size_t size() const;

private:
  enum class CounterKind : std::uint8_t {
    kRequest,
    kWatch,
    kSubscription,
  };

  struct SessionRecord {
    SessionSnapshot snapshot;
  };

  Status reserve(SessionId id, CounterKind kind);
  Status release(SessionId id, CounterKind kind);
  [[nodiscard]] std::size_t limit(CounterKind kind) const;
  static std::size_t &counter(SessionCounters &counters, CounterKind kind);

  ResourceLimits limits_;
  mutable std::mutex mutex_;
  std::unordered_map<SessionId, SessionRecord> sessions_;
  SessionId next_session_id_{1};
  bool draining_{false};
};

} // namespace pdcm

#endif // PDCM_CORE_SESSION_MANAGER_HPP_
