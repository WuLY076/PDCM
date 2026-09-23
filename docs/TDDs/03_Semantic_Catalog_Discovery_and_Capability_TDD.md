# TDD-03：SemanticCatalog、Discovery 与 Capability

> **TDD Version**：v0.8.2-tdd.1  
> **Baseline RFC**：PDCM Architecture Draft v0.8.2  
> **Status**：Proposed；P0 Metric Catalog 为 BLOCKED_EXTERNAL

## 1. Scope

本文冻结 SemanticCatalog 的层次、单卡 Entity、PDCM Device ID、generation、MetricDescriptor schema、Firmware heartbeat Health Catalog、Discovery、Capability 计算、原子更新和查询快照。本文不冻结具体 PDRL 函数、P0 Metric 列表或原生字段映射。

## 2. Design Objectives

- 上层只看到稳定 PDCM 语义，不看到 PDRL 类型；
- P0 明确区分 0 卡、1 卡和不支持的多卡配置；
- Catalog 和 Discovery 更新对所有读者原子可见；
- Capability 由目标 Catalog、Provider facts 和当前可用性的交集产生；
- generation 变化后旧引用不能指向新的设备实例；
- AC-3 未解阻时不创建虚构 Required Metrics；
- Diagnostic Catalog 不存在于 P0。

## 3. Catalog Layers

SemanticCatalog 由四层组成：

1. Built-in schema：Entity、Metric、Observation、Capability、Health 和错误枚举。
2. Target catalog：FPGA/EMU 的 P0 Metric 声明与 Firmware heartbeat Health 声明。
3. Provider descriptor：Provider 初始化后报告的 Device、Metric 和限制。
4. Runtime overlay：当前 availability、generation、degraded reason 和临时限制。

后层可以收窄能力或补充运行状态，不能改变已发布 Metric 的类型、单位或语义。Built-in schema 损坏为 Core Fatal；Provider descriptor 不可信时拒绝该次 commit，保留旧快照并使 Core Degraded。

## 4. P0 Entity Model

| Kind | P0 可见性 | 说明 |
|---|---|---|
| DEVICE | Public Required | 唯一 P0 可管理硬件实体；正常数量为 0 或 1 |
| NODE | Internal scope only | 可用于 service/capacity 统计，不作为 AC-2 Device |
| CHIP、CORE、PORT | Post-P0 | P0 不从数量或字符串推断，不对外发布 |

逻辑记录：

~~~cpp
struct EntityRecord {
  EntityKind kind;
  PdcmEntityId pdcm_id;
  uint64_t generation;
  EntityState state;
  AttributeMap attributes;
  ProviderSource source;
  ItemStatus status;
  NativeIdentity internal_native_identity;
};
~~~

NativeIdentity 只能存在于 Catalog 内部记录和 Provider boundary 以下。公共结果可包含经过规范化、长度受限的 native ID 副本。

## 5. Device Identity 与 ID

单卡 P0 的 identity key 优先级：

1. PDRL 明确声明稳定的 Device UUID；
2. target + PCI BDF + PDRL stable native ID；
3. PDRL 书面保证稳定的其他最小组合。

规则：

- 第一张且唯一受支持 Device 的 pdcm_id 固定从 0 开始；
- 同一 daemon 生命周期内，同一 identity key 重新出现时复用 pdcm_id 并按规则推进 generation；
- daemon restart 后可尽量稳定复现 ID，但公共契约只保证 generation 内有效；
- identity key 缺失或冲突时该 Device 为 ERROR，不得以枚举下标代替；
- 发现多于 1 个可管理 Device 时不为其中任何一个发布受管 Entity。

## 6. Generation

- catalog_generation：任何公开 Entity 集合、Descriptor、Capability 或 Target Catalog 变化时递增。
- entity.generation：Driver reload、外部 reset/replug、lost/reappear 或 incarnation 改变时递增。
- 单纯属性刷新只递增 catalog_generation。
- Provider 的一次瞬时重试不自动改变 entity generation。
- generation 使用 uint64；溢出为 Fatal invariant。

Watch、Query 和 Health target 必须保存完整 EntityRef。执行前发现 generation 不匹配时返回 STALE_GENERATION，禁止自动绑定新实例。

## 7. MetricDescriptor Schema

每个公开 Metric 必须包含：

| Field | 规则 |
|---|---|
| metric_id | PDCM 稳定 uint32，不等同 PDRL native enum |
| name | lower_snake_case；发布后不可复用 |
| value_type | int/uint/double/bool/string/enum 中之一 |
| unit | 规范单位；未知单位不能进入 REQUIRED |
| temporality | GAUGE 或 CUMULATIVE_COUNTER |
| scope | P0 必须为 DEVICE |
| collection_mode | POLL、EVENT 或 DERIVED |
| default_period_ns | 目标默认周期 |
| min_period_ns | 允许请求的最小周期 |
| freshness_ns | Query 默认 freshness |
| target_support | FPGA/EMU 独立状态 |
| requirement_level | REQUIRED 或 CONDITIONAL；POST_P0 不发布 |
| semantic_version | 语义变化时递增 |
| native_mapping | Internal only；由 TDD-08 冻结 |

Metric ID 删除后永久 reserved。Derived Metric 必须列出 dependency IDs 和 processor version。

当前版本不在本文填写任何具体 P0 Metric 行。PDRL/平台冻结列表前：

- AC-3 状态为 BLOCKED_EXTERNAL；
- target catalog 的 metrics 数组保持显式 pending，而不是填入推测值；
- CLI/API schema 可以实现，但不能在 release report 中宣称支持任何 Required Metric；
- Mock-only ID 必须位于测试命名空间，禁止进入发布 Catalog。

## 8. Firmware Heartbeat Health Catalog

P0 只允许一个 Health Subsystem：

| Field | 值 |
|---|---|
| subsystem | PDCM_HEALTH_SUBSYSTEM_FIRMWARE_HEARTBEAT |
| target | DEVICE |
| requirement | REQUIRED for approved FPGA/EMU target |
| evidence semantic | PDRL 正式暴露的 Firmware heartbeat |
| evaluation | TDD-06 |
| native mapping | TDD-08 待冻结 |
| unsupported behavior | item UNSUPPORTED + Health UNKNOWN + catalog gap |

Health Catalog 必须包含 evidence freshness、source、支持 target、semantic version 和 limitation 文本。具体 native symbol、结构和错误码不得写入本文件。

P0 不存在 Diagnostic Catalog。

## 9. Catalog Source

Catalog 使用 repository-owned YAML，经 schema validation 生成只读资源。生产 daemon 不接受任意外部 YAML 覆盖 Required 语义。

~~~yaml
catalog_version: 1
target: FPGA
metrics_status: BLOCKED_EXTERNAL
metrics: []
health:
  - subsystem: firmware_heartbeat
    requirement: REQUIRED
    evidence_contract: firmware_heartbeat
    native_mapping: TBD_TDD08
~~~

Metric 解阻后，每个条目必须通过以下校验：

- ID/name 唯一；
- type/unit/temporality 有效；
- min_period <= default_period <= freshness；
- Required raw Metric 有批准的 Provider mapping；
- Derived dependency 存在且无环；
- FPGA/EMU 支持状态明确；
- semantic version 和变更记录存在。

## 10. ProviderDescriptor 逻辑契约

ProviderManager 向 Catalog 提交不可变快照：

~~~cpp
struct ProviderDescriptor {
  ProviderVersion version;
  TargetKind target;
  ProviderState state;
  std::vector<ProviderEntity> entities;
  std::vector<ProviderMetric> metrics;
  std::vector<ProviderCapability> capabilities;
  std::vector<ItemError> errors;
  uint32_t detected_device_count;
};
~~~

Descriptor 必须自包含；Catalog commit 期间不得回调 Provider。原生指针、handle、allocator-owned memory 和 PDRL enum 不得出现。

具体字段、ABI 和构造方式待 TDD-08 补齐；本节只冻结 SemanticCatalog 所需的逻辑输入。

## 11. Discovery Algorithm

~~~mermaid
sequenceDiagram
  participant C as Core
  participant P as ProviderManager
  participant S as SemanticCatalog
  participant W as WatchManager
  C->>P: discover snapshot
  P-->>C: descriptor + detected count
  C->>S: prepare
  S->>S: validate identity, count, mapping
  alt zero or one device and descriptor valid
    S->>S: build immutable next snapshot
    S->>S: atomic commit generation + 1
    S-->>W: CatalogChanged
  else more than one device
    S->>S: publish unsupported topology overlay
    S-->>C: UNSUPPORTED + detected count
  else descriptor invalid
    S-->>C: reject commit and preserve old snapshot
  end
~~~

步骤：

1. 校验 descriptor 的结构、target 和 detected count。
2. detected count = 0：提交空 Device 集合，Discovery 返回 SUCCESS。
3. detected count = 1：规范化 identity、PCI BDF、状态和版本；构造 Entity。
4. detected count > 1：不挑选、不排序、不发布受管 Device；记录 topology unsupported。
5. 将 target Metric Catalog 与 provider-reported metrics 按显式 mapping 求交集。
6. 计算 heartbeat 和 Metric Capability。
7. 创建完整不可变 snapshot 和 diff。
8. 单次 atomic pointer swap 发布。
9. 发布 CatalogChanged；下游异步响应。

## 12. AC-2 Inventory

单卡 Required fields：

- Device count；
- PDCM Device ID；
- PDRL native ID 的规范化副本；
- PCI BDF；
- Device state；
- PDRV version。

Conditional fields：

- UUID/Serial；
- Device memory capacity；
- PFW version；
- HW version。

Conditional 字段不支持时必须返回 UNSUPPORTED；暂时无法读取时返回 NOT_AVAILABLE。不得使用空字符串、0 或旧值 + VALID 伪装。

## 13. Capability

最终能力：

~~~text
target_catalog_allowed
INTERSECT provider_reported
INTERSECT runtime_available
INTERSECT permission_visible
MINUS post_p0_disabled
~~~

Capability item 至少包含 kind、ID、supported、reason、limits、semantic version 和 catalog generation。

固定 reason：

- SUPPORTED；
- CATALOG_BLOCKED_EXTERNAL；
- PROVIDER_UNSUPPORTED；
- DEPENDENCY_MISSING；
- TEMPORARILY_UNAVAILABLE；
- PERMISSION_HIDDEN；
- POST_P0_DISABLED；
- TOPOLOGY_UNSUPPORTED。

P0 CapabilitySet 永远不包含 Operation。PRT Runtime/HLO/Run 和其他 Health Subsystem 不能通过版本推断为可用。

## 14. Reader API

~~~cpp
class CatalogView {
 public:
  uint64_t generation() const;
  Result<EntityRecord> resolve(EntityRef) const;
  Span<const EntityRecord> list(EntityFilter) const;
  Result<MetricDescriptor> metric(MetricId) const;
  CapabilitySet capabilities(EntityRef, Principal) const;
};

class SemanticCatalog {
 public:
  std::shared_ptr<const CatalogView> snapshot() const;
  CommitResult commit(ProviderDescriptor, TargetCatalog);
};
~~~

View 不可变。长生命周期对象只保存 ID/EntityRef，不长期持有旧 View；执行时重新 resolve。

## 15. Result Semantics

| 场景 | 结果 |
|---|---|
| Provider READY、0 Device | SUCCESS + count 0 |
| 1 Device 完整 | SUCCESS + count 1 |
| 1 Device 部分属性失败 | Device 可返回，调用 PARTIAL_RESULT |
| 多于 1 Device | UNSUPPORTED + detected count；entities 不填充 |
| Provider UNAVAILABLE、从未成功发现 | UNAVAILABLE；无 Device |
| Provider UNAVAILABLE、有旧快照 | 可返回带 unavailable/stale 标记的诊断视图，不可标 VALID |
| identity 缺失/冲突 | Item ERROR；不使用枚举下标修复 |
| old EntityRef | STALE_GENERATION |
| Required Metric 未冻结 | CATALOG_BLOCKED_EXTERNAL |

lost Device 可在内部 Tombstone 中保留用于 generation 和排障；默认 public list 不返回 lost Device。

## 16. CatalogChanged

事件包含 old/new catalog generation 及 compact diff：added/removed/changed entity、descriptor、capability。

- WatchManager retire/recompile plan；
- DataManager 将旧 generation latest 标为 stale/tombstone；
- MetricsManager 重建 processor dependency graph 和 heartbeat state；
- SubscriptionManager 发布 entity/capability change；
- 不通知任何 OperationManager。

## 17. Thread Safety

- 单 writer mutex 保护 prepare/commit；
- reader 通过 atomic shared immutable snapshot；
- commit 不在锁内执行 Provider call、日志 I/O 或订阅投递；
- identity table 由 Catalog writer 独占；
- production catalog 不支持 runtime patch。

## 18. Tests

### 18.1 Schema

- ID/name/unit/period/semantic version；
- metrics_status=BLOCKED_EXTERNAL 时禁止 Required Metric 发布；
- Health Catalog 只能有 Firmware heartbeat；
- Diagnostic Catalog 或 Operation Capability 出现即失败。

### 18.2 Discovery

- 0 Device SUCCESS；
- 1 Device baseline/conditional fields；
- 2+ Device UNSUPPORTED 且没有受管 Entity；
- reordered native enumeration 不改变单卡 ID；
- identity missing/collision；
- malformed descriptor 原子拒绝。

### 18.3 Generation 与 Atomicity

- reload/reset/replug/lost-reappear 递增 entity generation；
- 属性变化只递增 catalog generation；
- 并发 reader 只看到完整 old/new；
- rejected commit 不改变当前 snapshot。

### 18.4 Acceptance IDs

| ID | Requirement |
|---|---|
| SEM-ACC-01 | AC-2 baseline inventory 可追溯到 Provider source |
| SEM-ACC-02 | 0/1/>1 Device 行为完全符合单卡范围 |
| SEM-ACC-03 | Capability 不由型号/版本字符串猜测 |
| SEM-ACC-04 | reload/reset 后旧 EntityRef 为 STALE_GENERATION |
| SEM-ACC-05 | Metric 未冻结时 AC-3 保持 BLOCKED_EXTERNAL |
| SEM-ACC-06 | P0 Catalog 不含 Diagnostic 或额外 Health subsystem |

## 19. Completion Criteria

- 单卡 Entity、ID、generation 和 Discovery schema 评审完成；
- Firmware heartbeat Health Catalog 的逻辑语义冻结；
- P0 Metric Catalog 未冻结前文档明确保持 BLOCKED_EXTERNAL；
- MockProvider 0/1/>1、partial、lost、reload fixtures 通过；
- 所有消费者只依赖 immutable CatalogView；
- PDRL 类型只允许出现在 TDD-08 最终批准的实现目录。

