#ifndef PDCM_SEMANTIC_SEMANTIC_CATALOG_HPP_
#define PDCM_SEMANTIC_SEMANTIC_CATALOG_HPP_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "semantic/catalog_types.hpp"

namespace pdcm {

class CatalogView {
public:
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] std::uint32_t detectedDeviceCount() const noexcept;
  [[nodiscard]] bool topologyUnsupported() const noexcept;
  [[nodiscard]] const std::vector<EntityRecord> &entities() const noexcept;
  [[nodiscard]] const std::vector<CapabilitySet> &
  capabilitySets() const noexcept;
  [[nodiscard]] EntityResolveResult resolve(EntityRef entity) const;
  [[nodiscard]] CapabilityQueryResult capabilities(EntityRef entity) const;

private:
  friend class SemanticCatalog;

  std::uint64_t generation_{0};
  std::uint32_t detected_device_count_{0};
  bool topology_unsupported_{false};
  std::vector<EntityRecord> entities_;
  std::vector<CapabilitySet> capability_sets_;
};

class SemanticCatalog {
public:
  explicit SemanticCatalog(TargetCatalog target_catalog);

  SemanticCatalog(const SemanticCatalog &) = delete;
  SemanticCatalog &operator=(const SemanticCatalog &) = delete;

  [[nodiscard]] std::shared_ptr<const CatalogView> snapshot() const noexcept;
  CatalogCommitResult commit(const ProviderDescriptor &descriptor);

private:
  struct IdentityHistory {
    EntityId id;
    std::uint64_t generation{0};
    std::string incarnation;
    bool present{false};
  };

  [[nodiscard]] std::string identityKey(const ProviderEntity &entity) const;
  [[nodiscard]] CapabilitySet
  buildCapabilities(const EntityRecord &entity,
                    const ProviderDescriptor &descriptor,
                    std::uint64_t catalog_generation) const;

  TargetCatalog target_catalog_;
  mutable std::mutex writer_mutex_;
  std::shared_ptr<const CatalogView> snapshot_;
  std::map<std::string, IdentityHistory> identity_history_;
  std::uint64_t next_entity_id_{0};
};

} // namespace pdcm

#endif // PDCM_SEMANTIC_SEMANTIC_CATALOG_HPP_
