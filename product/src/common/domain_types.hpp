#ifndef PDCM_COMMON_DOMAIN_TYPES_HPP_
#define PDCM_COMMON_DOMAIN_TYPES_HPP_

#include <cstdint>
#include <string>
#include <variant>

namespace pdcm {

struct EntityId {
  std::uint64_t value{0};

  friend bool operator==(EntityId lhs, EntityId rhs) noexcept {
    return lhs.value == rhs.value;
  }
  friend bool operator!=(EntityId lhs, EntityId rhs) noexcept {
    return !(lhs == rhs);
  }
};

struct MetricId {
  std::uint32_t value{0};

  friend bool operator==(MetricId lhs, MetricId rhs) noexcept {
    return lhs.value == rhs.value;
  }
  friend bool operator!=(MetricId lhs, MetricId rhs) noexcept {
    return !(lhs == rhs);
  }
};

enum class EntityKind : std::uint32_t {
  kUnknown = 0,
  kDevice = 1,
  kNode = 2,
};

struct EntityRef {
  EntityKind kind{EntityKind::kUnknown};
  EntityId id{};
  std::uint64_t generation{0};

  friend bool operator==(const EntityRef &lhs, const EntityRef &rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.id == rhs.id &&
           lhs.generation == rhs.generation;
  }
  friend bool operator!=(const EntityRef &lhs, const EntityRef &rhs) noexcept {
    return !(lhs == rhs);
  }
};

using MetricValue =
    std::variant<std::int64_t, std::uint64_t, double, bool, std::string>;

} // namespace pdcm

#endif // PDCM_COMMON_DOMAIN_TYPES_HPP_
