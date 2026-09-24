#include "semantic/semantic_catalog.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace pdcm {
namespace {

bool validMetricName(const std::string &name) {
  if (name.empty() ||
      (name.front() != '_' &&
       std::islower(static_cast<unsigned char>(name.front())) == 0)) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](const char value) {
    const unsigned char character = static_cast<unsigned char>(value);
    return std::islower(character) != 0 || std::isdigit(character) != 0 ||
           value == '_';
  });
}

StringAttribute validAttribute(std::string value) {
  return StringAttribute{ObservationStatus::kValid, std::move(value)};
}

StringAttribute unavailableAttribute() {
  return StringAttribute{ObservationStatus::kNotAvailable, {}};
}

EntityState entityState(const ProviderEntityState state) {
  switch (state) {
  case ProviderEntityState::kReady:
    return EntityState::kReady;
  case ProviderEntityState::kError:
    return EntityState::kError;
  case ProviderEntityState::kUnknown:
  case ProviderEntityState::kLost:
    return EntityState::kUnknown;
  }
  return EntityState::kUnknown;
}

bool sameCapabilitySemantics(const CapabilitySet &lhs,
                             const CapabilitySet &rhs) {
  if (lhs.entity != rhs.entity || lhs.items.size() != rhs.items.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.items.size(); ++index) {
    const CapabilityItem &left = lhs.items[index];
    const CapabilityItem &right = rhs.items[index];
    if (left.kind != right.kind || left.id != right.id ||
        left.supported != right.supported || left.reason != right.reason ||
        left.semantic_version != right.semantic_version) {
      return false;
    }
  }
  return true;
}

CatalogDiff difference(const CatalogView &old_view,
                       const CatalogView &new_view) {
  CatalogDiff diff;
  diff.old_generation = old_view.generation();
  diff.new_generation = new_view.generation();

  for (const EntityRecord &entity : new_view.entities()) {
    const auto old =
        std::find_if(old_view.entities().begin(), old_view.entities().end(),
                     [&entity](const EntityRecord &candidate) {
                       return candidate.ref.kind == entity.ref.kind &&
                              candidate.ref.id == entity.ref.id;
                     });
    if (old == old_view.entities().end()) {
      ++diff.added_entities;
    } else if (!(*old == entity)) {
      ++diff.changed_entities;
    }
  }
  for (const EntityRecord &entity : old_view.entities()) {
    const auto current =
        std::find_if(new_view.entities().begin(), new_view.entities().end(),
                     [&entity](const EntityRecord &candidate) {
                       return candidate.ref.kind == entity.ref.kind &&
                              candidate.ref.id == entity.ref.id;
                     });
    if (current == new_view.entities().end()) {
      ++diff.removed_entities;
    }
  }

  if (old_view.capabilitySets().size() != new_view.capabilitySets().size()) {
    diff.capabilities_changed = true;
  } else {
    for (std::size_t index = 0; index < old_view.capabilitySets().size();
         ++index) {
      if (!sameCapabilitySemantics(old_view.capabilitySets()[index],
                                   new_view.capabilitySets()[index])) {
        diff.capabilities_changed = true;
        break;
      }
    }
  }
  diff.topology_changed =
      old_view.topologyUnsupported() != new_view.topologyUnsupported() ||
      old_view.detectedDeviceCount() != new_view.detectedDeviceCount();
  return diff;
}

} // namespace

Status TargetCatalog::validate() const {
  if (catalog_version == 0 ||
      (target != TargetKind::kFpga && target != TargetKind::kEmu)) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "target catalog header is invalid");
  }
  if (metrics_status == MetricsCatalogStatus::kBlockedExternal &&
      !metrics.empty()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "blocked metric catalog must not publish metrics");
  }

  std::set<std::uint32_t> metric_ids;
  std::set<std::string> metric_names;
  for (const MetricDescriptor &metric : metrics) {
    if (metric.id.value == 0 || !validMetricName(metric.name) ||
        metric.unit.empty() || metric.scope != EntityKind::kDevice ||
        metric.semantic_version == 0 || metric.min_period_ns == 0 ||
        metric.default_period_ns < metric.min_period_ns ||
        metric.freshness_ns < metric.default_period_ns ||
        !metric_ids.insert(metric.id.value).second ||
        !metric_names.insert(metric.name).second) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "metric descriptor is invalid");
    }
    if (metric.requirement == RequirementLevel::kRequired &&
        metric.collection_mode != MetricCollectionMode::kDerived &&
        !metric.provider_mapping_approved) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "required raw metric lacks an approved mapping");
    }
  }

  if (health.size() != 1 ||
      health.front().subsystem_id != kFirmwareHeartbeatHealthId ||
      health.front().scope != EntityKind::kDevice ||
      health.front().requirement != RequirementLevel::kRequired ||
      health.front().evidence_contract != "firmware_heartbeat" ||
      health.front().freshness_ns == 0 ||
      health.front().semantic_version == 0 ||
      health.front().limitation.empty()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "firmware heartbeat health catalog is invalid");
  }
  return Status::success();
}

TargetCatalog TargetCatalog::blocked(const TargetKind target) {
  TargetCatalog catalog;
  catalog.target = target;
  catalog.metrics_status = MetricsCatalogStatus::kBlockedExternal;
  HealthCatalogEntry heartbeat;
  heartbeat.freshness_ns = UINT64_C(5000000000);
  catalog.health.push_back(std::move(heartbeat));
  return catalog;
}

std::uint64_t CatalogView::generation() const noexcept { return generation_; }

std::uint32_t CatalogView::detectedDeviceCount() const noexcept {
  return detected_device_count_;
}

bool CatalogView::topologyUnsupported() const noexcept {
  return topology_unsupported_;
}

const std::vector<EntityRecord> &CatalogView::entities() const noexcept {
  return entities_;
}

const std::vector<CapabilitySet> &CatalogView::capabilitySets() const noexcept {
  return capability_sets_;
}

EntityResolveResult CatalogView::resolve(const EntityRef entity) const {
  if (entity.kind != EntityKind::kDevice) {
    return {Status(PDCM_STATUS_INVALID_ARGUMENT,
                   "only device entities are public in P0"),
            std::nullopt};
  }

  const auto match = std::find_if(
      entities_.begin(), entities_.end(), [entity](const EntityRecord &record) {
        return record.ref.kind == entity.kind && record.ref.id == entity.id;
      });
  if (match == entities_.end()) {
    return {Status(PDCM_STATUS_NOT_FOUND, "entity is not present"),
            std::nullopt};
  }
  if (match->ref.generation != entity.generation) {
    return {Status(PDCM_STATUS_STALE_GENERATION, "entity generation is stale"),
            std::nullopt};
  }
  return {Status::success(), *match};
}

CapabilityQueryResult CatalogView::capabilities(const EntityRef entity) const {
  const EntityResolveResult resolved = resolve(entity);
  if (!resolved.status.ok()) {
    return {resolved.status, std::nullopt};
  }
  const auto match = std::find_if(
      capability_sets_.begin(), capability_sets_.end(),
      [entity](const CapabilitySet &set) { return set.entity == entity; });
  if (match == capability_sets_.end()) {
    return {
        Status(PDCM_STATUS_NOT_FOUND, "entity capability set is not present"),
        std::nullopt};
  }
  return {Status::success(), *match};
}

SemanticCatalog::SemanticCatalog(TargetCatalog target_catalog)
    : target_catalog_(std::move(target_catalog)) {
  const Status validation = target_catalog_.validate();
  if (!validation.ok()) {
    throw std::invalid_argument(validation.message());
  }
  snapshot_ = std::make_shared<const CatalogView>();
}

std::shared_ptr<const CatalogView> SemanticCatalog::snapshot() const noexcept {
  return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
}

CatalogCommitResult
SemanticCatalog::commit(const ProviderDescriptor &descriptor) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  const std::shared_ptr<const CatalogView> previous = snapshot();

  CatalogCommitResult result;
  result.detected_device_count = descriptor.detected_device_count;
  result.catalog_generation = previous->generation();

  if (descriptor.target != target_catalog_.target ||
      descriptor.state != ProviderState::kReady) {
    result.status = Status(PDCM_STATUS_INVALID_ARGUMENT,
                           "provider descriptor target or state is invalid");
    return result;
  }

  std::vector<const ProviderEntity *> manageable;
  manageable.reserve(descriptor.entities.size());
  for (const ProviderEntity &entity : descriptor.entities) {
    if (entity.manageable) {
      manageable.push_back(&entity);
    }
  }
  if (manageable.size() != descriptor.detected_device_count) {
    result.status =
        Status(PDCM_STATUS_INVALID_ARGUMENT,
               "detected device count does not match provider entities");
    return result;
  }

  std::string selected_identity;
  bool selected_was_present = false;
  if (descriptor.detected_device_count == 1) {
    selected_identity = identityKey(*manageable.front());
    const auto previous_identity = identity_history_.find(selected_identity);
    selected_was_present = previous_identity != identity_history_.end() &&
                           previous_identity->second.present;
    if (selected_identity.empty()) {
      result.status = Status(PDCM_STATUS_INVALID_ARGUMENT,
                             "single device lacks a stable identity key");
      return result;
    }
  }

  if (previous->generation() == std::numeric_limits<std::uint64_t>::max()) {
    result.status = Status(PDCM_STATUS_INTERNAL, "catalog generation overflow");
    return result;
  }

  auto next = std::make_shared<CatalogView>();
  next->generation_ = previous->generation() + 1;
  next->detected_device_count_ = descriptor.detected_device_count;
  next->topology_unsupported_ = descriptor.detected_device_count > 1;
  auto next_identity_history = identity_history_;
  std::uint64_t next_entity_id = next_entity_id_;

  for (auto &entry : next_identity_history) {
    entry.second.present = false;
  }

  pdcm_status_t commit_status = PDCM_STATUS_SUCCESS;
  if (descriptor.detected_device_count == 1) {
    const ProviderEntity &provider_entity = *manageable.front();
    auto history = next_identity_history.find(selected_identity);
    if (history == next_identity_history.end()) {
      if (next_entity_id == std::numeric_limits<std::uint64_t>::max()) {
        result.status = Status(PDCM_STATUS_INTERNAL, "entity ID overflow");
        return result;
      }
      IdentityHistory created;
      created.id = EntityId{next_entity_id++};
      created.generation = 1;
      created.incarnation = provider_entity.incarnation;
      created.present = true;
      history =
          next_identity_history.emplace(selected_identity, std::move(created))
              .first;
    } else {
      if (!selected_was_present ||
          history->second.incarnation != provider_entity.incarnation) {
        if (history->second.generation ==
            std::numeric_limits<std::uint64_t>::max()) {
          result.status =
              Status(PDCM_STATUS_INTERNAL, "entity generation overflow");
          return result;
        }
        ++history->second.generation;
      }
      history->second.incarnation = provider_entity.incarnation;
      history->second.present = true;
    }

    EntityRecord entity;
    entity.ref = EntityRef{EntityKind::kDevice, history->second.id,
                           history->second.generation};
    entity.state = entityState(provider_entity.state);
    entity.native_id = validAttribute(provider_entity.stable_native_id);
    entity.pci_bdf = validAttribute(provider_entity.pci_bdf);
    entity.pdrv_version = provider_entity.pdrv_version.empty()
                              ? unavailableAttribute()
                              : validAttribute(provider_entity.pdrv_version);
    entity.provider_version = descriptor.provider_version;
    entity.target = descriptor.target;
    if (provider_entity.state != ProviderEntityState::kReady ||
        entity.pdrv_version.status != ObservationStatus::kValid ||
        !descriptor.errors.empty()) {
      entity.item_status = PDCM_STATUS_PARTIAL_RESULT;
      commit_status = PDCM_STATUS_PARTIAL_RESULT;
    }

    next->entities_.push_back(entity);
    next->capability_sets_.push_back(
        buildCapabilities(entity, descriptor, next->generation_));
  } else if (descriptor.detected_device_count > 1) {
    commit_status = PDCM_STATUS_UNSUPPORTED;
  }

  result.diff = difference(*previous, *next);
  result.status = Status(
      commit_status, commit_status == PDCM_STATUS_UNSUPPORTED
                         ? "multiple manageable devices are unsupported in P0"
                     : commit_status == PDCM_STATUS_PARTIAL_RESULT
                         ? "discovery completed with partial attributes"
                         : "");
  result.catalog_generation = next->generation_;
  result.committed = true;
  identity_history_ = std::move(next_identity_history);
  next_entity_id_ = next_entity_id;
  std::atomic_store_explicit(&snapshot_,
                             std::static_pointer_cast<const CatalogView>(next),
                             std::memory_order_release);
  return result;
}

std::string SemanticCatalog::identityKey(const ProviderEntity &entity) const {
  if (entity.stable_native_id.empty() || entity.pci_bdf.empty()) {
    return {};
  }
  return std::to_string(static_cast<unsigned int>(target_catalog_.target)) +
         "|" + entity.pci_bdf + "|" + entity.stable_native_id;
}

CapabilitySet SemanticCatalog::buildCapabilities(
    const EntityRecord &entity, const ProviderDescriptor &descriptor,
    const std::uint64_t catalog_generation) const {
  CapabilitySet set;
  set.entity = entity.ref;
  set.catalog_generation = catalog_generation;

  CapabilityItem metrics;
  metrics.kind = CapabilityKind::kMetricsCatalog;
  metrics.reason =
      target_catalog_.metrics_status == MetricsCatalogStatus::kBlockedExternal
          ? CapabilityReason::kCatalogBlockedExternal
          : CapabilityReason::kSupported;
  metrics.supported =
      target_catalog_.metrics_status == MetricsCatalogStatus::kReady;
  metrics.semantic_version = target_catalog_.catalog_version;
  metrics.catalog_generation = catalog_generation;
  set.items.push_back(metrics);

  for (const MetricDescriptor &descriptor_entry : target_catalog_.metrics) {
    CapabilityItem metric;
    metric.kind = CapabilityKind::kMetric;
    metric.id = descriptor_entry.id.value;
    metric.semantic_version = descriptor_entry.semantic_version;
    metric.catalog_generation = catalog_generation;
    const auto provider = std::find_if(
        descriptor.capabilities.begin(), descriptor.capabilities.end(),
        [&descriptor_entry](const ProviderCapability &candidate) {
          return candidate.kind == ProviderDataKind::kMetric &&
                 candidate.data_id == descriptor_entry.id.value;
        });
    metric.supported =
        provider != descriptor.capabilities.end() && provider->supported;
    metric.reason = metric.supported ? CapabilityReason::kSupported
                                     : CapabilityReason::kProviderUnsupported;
    set.items.push_back(metric);
  }

  const HealthCatalogEntry &health_catalog = target_catalog_.health.front();
  CapabilityItem health;
  health.kind = CapabilityKind::kHealth;
  health.id = health_catalog.subsystem_id;
  health.semantic_version = health_catalog.semantic_version;
  health.catalog_generation = catalog_generation;
  if (!health_catalog.provider_data_id.has_value()) {
    health.reason = CapabilityReason::kDependencyMissing;
  } else {
    const auto provider = std::find_if(
        descriptor.capabilities.begin(), descriptor.capabilities.end(),
        [&health_catalog](const ProviderCapability &candidate) {
          return candidate.kind == ProviderDataKind::kHeartbeatEvidence &&
                 candidate.data_id == *health_catalog.provider_data_id;
        });
    health.supported =
        provider != descriptor.capabilities.end() && provider->supported;
    health.reason = health.supported ? CapabilityReason::kSupported
                                     : CapabilityReason::kProviderUnsupported;
  }
  set.items.push_back(health);
  return set;
}

} // namespace pdcm
