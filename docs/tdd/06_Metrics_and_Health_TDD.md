# TDD-06：MetricsManager、Metric Processor 与 Firmware Heartbeat Health

> **TDD Version**：v0.8.2-tdd.1  
> **Baseline RFC**：PDCM Architecture Draft v0.8.2  
> **Status**：Proposed；具体 P0 Metrics 与 native heartbeat mapping 为 BLOCKED_EXTERNAL

## 1. Scope

本文冻结 MetricsManager 的域边界、Metric processor 通用模型、derived metric 规则，以及 P0 唯一 Health Subsystem——Firmware heartbeat——的 evidence、状态映射、freshness、查询和事件语义。物理采集见 TDD-04，数据存储见 TDD-05，具体 PDRL 映射待 TDD-08。

## 2. MetricsManager Boundary

MetricsManager 负责：

- 解析和校验 Metric/Health 请求；
- 将逻辑采集需求交给 WatchManager；
- 通过 QueryEngine 获取 raw/derived Observation；
- 管理 processor registry 和 dependency graph；
- 接收 DataCommitted/CatalogChanged；
- 将 derived Observation 和 HealthResult 写回 DataManager；
- 管理 Metric/Health subscription filter。

MetricsManager 禁止：

- 直接调用 Provider 或 PDRL；
- 持有第二份长期 Cache；
- 运行物理采集 timer；
- 绕过 QueryEngine 读取任意数据；
- 实现 Policy、Config、Reset、Diagnostic 或额外 Health Subsystem。

## 3. Internal Interface

~~~cpp
class MetricsManager {
 public:
  Result<WatchHandle> createWatch(SessionContext, MetricWatchRequest);
  Result<QueryResult> query(SessionContext, MetricQueryRequest);
  Result<HealthResultSet> queryHealth(SessionContext, HealthRequest);
  Result<SubscriptionHandle> subscribe(
      SessionContext, MetricsSubscriptionRequest);
  void onDataCommitted(const DataCommitEvent&);
  void onCatalogChanged(const CatalogDiff&);
  void onProviderStateChanged(const ProviderStateEvent&);
};
~~~

SessionManager 记录 ownership；MetricsManager 只返回 domain resource ID。

## 4. Processor Model

~~~cpp
class MetricProcessor {
 public:
  virtual ProcessorId id() const = 0;
  virtual Span<const MetricId> inputs() const = 0;
  virtual Span<const MetricId> outputs() const = 0;
  virtual Result<std::vector<Observation>> evaluate(
      const ProcessorContext&, const ObservationSnapshot&) = 0;
};
~~~

允许的 P0 processor：

| Type | 用途 |
|---|---|
| normalization | 只有不能在 Provider boundary 完成的稳定跨源转换 |
| counter delta/rate | Catalog 声明的 derived counter |
| aggregation | Catalog 明确声明的单卡时间窗口统计 |
| FirmwareHeartbeatHealthProcessor | 唯一 Health processor |

processor 由编译时 registry 和 target catalog 启用，不支持 runtime plugin。

## 5. Metric Catalog Dependency

具体 P0 Metric ID、名称、类型、单位、周期、freshness、PDRL mapping 尚未冻结。冻结前：

- MetricsManager 可用测试命名空间 Metric 验证框架；
- 生产 Catalog 不注册任何推测的 Required Metric；
- AC-3 和 AC-7 的 dynamic Metric 部分保持 BLOCKED_EXTERNAL；
- dmon 可以完成命令/协议开发，但目标验收不能 PASS；
- Heartbeat Health 不能被普通 Metric 列表是否为空取消。

Metric Catalog 冻结后，必须为每个 Required Metric 增加 descriptor、processor dependency、query/watch vectors 和 golden comparison。

## 6. Dependency Graph

启动和 Catalog commit 时构建：

~~~text
raw Metric → processor → derived Metric
heartbeat evidence → FirmwareHeartbeatHealthProcessor → HealthResult
~~~

必须验证：

- output descriptor 存在且 collection_mode=DERIVED；
- 一个 output 只有一个 authoritative processor；
- 图无环；
- dependency semantic version 满足 processor；
- derivation depth <= 4；
- heartbeat processor 只依赖批准的 heartbeat evidence contract。

Required processor 无法建立时对应能力不可发布，Core 进入 Degraded 或目标发布被阻断。不得保留旧 generation 输出为 VALID。

## 7. Processor Scheduling

- DataCommitted 只携带 changed keys/epoch；
- 同一 processor/entity/input epoch set 去重；
- task 进入 bounded pool；
- 同一 entity/processor 串行；
- evaluate 前从 QueryEngine 取得 immutable snapshot；
- 输出包含 input sequences、processor version、catalog/entity generation 和 depth；
- input generation 不一致时丢弃输出并等待新数据；
- queue lag 导致 evidence 过期时 Health 转 UNKNOWN。

## 8. Derived Counter

只有以下条件同时满足才计算 delta/rate：

- 两个样本均 VALID；
- Entity generation、Metric semantic version 和 counter epoch 相同；
- 时间单调且间隔大于最小阈值；
- new >= old，或 Catalog 明确 wrap width 且满足 wrap 判定；
- gap 不超过 descriptor 上限。

否则输出 NOT_AVAILABLE，reason 为 insufficient samples、generation changed、counter reset、time invalid 或 gap too large。禁止把负差钳制为 0。

## 9. Metric Query

1. snapshot SemanticCatalog；
2. 展开当前单卡 selection；
3. 检查 capability 和 catalog status；
4. 对 derived Metric 建立 internal dependency Watch；
5. 由 QueryEngine 执行 cache/fresh policy；
6. 等待 processor 到 request deadline 或返回 NOT_AVAILABLE；
7. 保持 deterministic output order。

Fresh Read 只读取 raw dependencies，不向 Provider 发送 derived Metric ID。

当 Metric Catalog 为 BLOCKED_EXTERNAL 时，生产 Metric 请求返回 UNSUPPORTED 或明确 CATALOG_BLOCKED_EXTERNAL；不能返回空 SUCCESS 让调用方误判已采样。

## 10. P0 Health Scope

P0 唯一接受的 subsystem：

~~~text
PDCM_HEALTH_SUBSYSTEM_FIRMWARE_HEARTBEAT
~~~

以下均不得形成 P0 Health：

- Driver readiness；
- Device Access；
- PCIe Link；
- Device Memory；
- Temperature/Sensor；
- RAS；
- Provider/Core queue 或线程状态；
- Runtime workload。

服务/Provider 状态通过 Version、Readiness 和 error metadata 表达，不伪装成 Device Health。

## 11. Heartbeat Evidence

逻辑 evidence：

~~~cpp
struct FirmwareHeartbeatEvidence {
  EntityRef device;
  HeartbeatNativeClass native_class;
  ObservationStatus status;
  uint64_t sequence_or_token;
  int64_t source_sample_time_ns;
  int64_t observed_monotonic_time_ns;
  SourceMetadata source;
  ErrorMetadata error;
  uint64_t catalog_generation;
};
~~~

native_class 只表达规范化分类：

- NORMAL；
- WARNING；
- FAULT；
- UNKNOWN_NATIVE。

具体原生值、函数和转换表由 TDD-08 冻结。PDRL 未提供分类但提供可判定的 heartbeat token 时，如何识别正常/异常也必须在 TDD-08 与 Health Contract 中显式定义，禁止在实现中猜测。

## 12. Health State

公开状态：

- HEALTHY：存在当前 generation 的新鲜、有效、明确 NORMAL heartbeat；
- WARNING：存在新鲜、有效、明确 WARNING heartbeat；
- ERROR：存在新鲜、有效、明确 FAULT heartbeat；
- UNKNOWN：缺失、过期、timeout、读取失败、Provider unavailable、native classification 未知或尚未获得首个 heartbeat。

UNKNOWN 不是健康，也不自动等于硬件故障。

## 13. Evidence Mapping

| Evidence | Item status | Health state |
|---|---|---|
| VALID + fresh + NORMAL | VALID | HEALTHY |
| VALID + fresh + WARNING | VALID | WARNING |
| VALID + fresh + FAULT | VALID | ERROR |
| STALE/age > heartbeat freshness | STALE | UNKNOWN |
| NOT_AVAILABLE | NOT_AVAILABLE | UNKNOWN |
| timeout/read ERROR | ERROR | UNKNOWN |
| Provider UNAVAILABLE | ERROR/UNAVAILABLE context | UNKNOWN |
| heartbeat capability unsupported | UNSUPPORTED | UNKNOWN + catalog gap |
| old Entity generation | STALE_GENERATION | 不产生当前 HealthResult |
| UNKNOWN_NATIVE | ERROR or NOT_AVAILABLE per mapping | UNKNOWN |

通用读取超时不得转换为 ERROR Health；只有 PDRL 明确提供 FAULT 分类时才能返回 ERROR。

## 14. HealthResult

~~~cpp
struct HealthResult {
  EntityRef entity;
  HealthSubsystem subsystem;
  HealthState state;
  ItemStatus item_status;
  uint64_t catalog_generation;
  int64_t evaluated_monotonic_time_ns;
  int64_t evidence_age_ns;
  std::vector<EvidenceRef> evidence;
  std::vector<Limitation> limitations;
  StableHealthCode code;
};
~~~

P0 的 Device aggregate 等于 Firmware heartbeat result，不进行跨 subsystem worst-of 聚合。显式请求其他 subsystem 时只返回 UNSUPPORTED item，不加入 aggregate。

## 15. Freshness

Heartbeat Health Catalog 必须冻结有限 freshness H：

- H 必须 >= heartbeat collection period；
- age 以本地 monotonic observed time 计算；
- caller 可以要求更严格 max_age，但不能放宽 H；
- 到达 H 时，即使没有新数据 commit，也必须由 timer 触发 HEALTHY/WARNING/ERROR → UNKNOWN；
- daemon restart 后没有历史 Health 状态，初始为 UNKNOWN；
- old generation evidence 不能用于新 generation。

H 的具体目标值等待 PDRL heartbeat 周期确认，不在本版本臆测。

## 16. State Transitions

~~~mermaid
stateDiagram-v2
  [*] --> Unknown
  Unknown --> Healthy: fresh NORMAL
  Unknown --> Warning: fresh WARNING
  Unknown --> Error: fresh FAULT
  Healthy --> Warning: fresh WARNING
  Healthy --> Error: fresh FAULT
  Warning --> Error: fresh FAULT
  Warning --> Healthy: newer fresh NORMAL
  Error --> Warning: newer fresh WARNING
  Error --> Healthy: newer fresh NORMAL
  Healthy --> Unknown: stale, missing, timeout
  Warning --> Unknown: stale, missing, timeout
  Error --> Unknown: stale, missing, timeout
~~~

PDRL 的显式分类为 authoritative，PDCM 不自行发明 threshold、duration 或 debounce。恢复必须使用严格更新的 heartbeat token/sample time；重复同一 evidence 不能触发恢复。

## 17. Health Query

pdcm_health_query：

1. 校验只请求 Firmware heartbeat；
2. resolve 当前唯一 Device generation；
3. 读取 HealthStore 和对应 evidence；
4. 重新计算 age；
5. evidence 过期时结果强制 UNKNOWN；
6. read policy 允许时可等待 heartbeat internal Watch 的一次 Fresh Read；
7. deadline 前仍无确定 evidence 时返回 UNKNOWN；
8. 输出 evidence、age、code 和 limitation。

Health Query 不直接调用 PDRL。所有证据必须经 Watch/Collection/Data 通路进入。

## 18. Stable Health Codes

- PDCM_HEALTH_HEARTBEAT_OK；
- PDCM_HEALTH_HEARTBEAT_WARNING；
- PDCM_HEALTH_HEARTBEAT_FAULT；
- PDCM_HEALTH_HEARTBEAT_MISSING；
- PDCM_HEALTH_HEARTBEAT_STALE；
- PDCM_HEALTH_HEARTBEAT_TIMEOUT；
- PDCM_HEALTH_HEARTBEAT_READ_ERROR；
- PDCM_HEALTH_HEARTBEAT_UNSUPPORTED；
- PDCM_HEALTH_PROVIDER_UNAVAILABLE；
- PDCM_HEALTH_PROCESSOR_ERROR。

native code 只作为 evidence metadata，不直接成为公共 stable code。

## 19. Health Events

只在 state、stable code 或 limitation 变化时发布 HEALTH_CHANGED。事件包含 old/new state、evidence refs、age 和 Catalog generation。

- HEALTHY/WARNING/ERROR 转 UNKNOWN 必须发布；
- UNKNOWN 恢复到确定状态必须发布；
- 相同状态的新 evidence 不发状态变化事件；
- Event 不内嵌未限制大小的 native payload。

## 20. Concurrency 与 Backpressure

- processor graph 单 writer、readers 使用 immutable snapshot；
- per Device/processor 串行；
- Health timer 使用 monotonic clock；
- queue 满时 raw evidence 已安全保存，processor lag 被记录；
- lag 超过 H 时 Query 必须返回 UNKNOWN；
- Catalog/generation 变化清空 heartbeat processor state，从 UNKNOWN 重建。

## 21. Tests

### 21.1 Processor

- dependency cycle、missing output、semantic mismatch；
- counter normal/wrap/reset/generation/time gap；
- duplicate commit 去重；
- Catalog change race；
- BLOCKED_EXTERNAL Catalog 不发布 Required Metric。

### 21.2 Firmware Heartbeat Health

- 初始 UNKNOWN；
- fresh NORMAL/WARNING/FAULT 映射；
- stale/missing/timeout/read error/provider unavailable → UNKNOWN；
- other subsystem → UNSUPPORTED；
- timeout 不得变为 ERROR Health；
- newer normal evidence 恢复；
- duplicate/old generation evidence 不恢复；
- freshness timer 使用 fake clock；
- Device aggregate 等于 heartbeat。

### 21.3 Acceptance IDs

| ID | Requirement |
|---|---|
| MET-ACC-01 | Metric type/unit/semantic version 由冻结 Catalog 驱动 |
| MET-ACC-02 | derived Metric 不跨 generation/reset 计算 |
| HLT-ACC-01 | P0 只公开 Firmware heartbeat Health |
| HLT-ACC-02 | stale/missing/timeout/read failure 不返回 HEALTHY |
| HLT-ACC-03 | NORMAL/WARNING/FAULT 映射确定且可追溯 |
| HLT-ACC-04 | HealthResult 包含 evidence、age、limitation 和 stable code |
| HLT-ACC-05 | 其他 subsystem 返回 UNSUPPORTED |

## 22. Completion Criteria

- Heartbeat 状态机和 fake-clock vectors 全覆盖；
- 没有 Device Access、PCIe、Memory、Sensor、RAS 或 Driver Health；
- 没有 MetricsManager 直连 Provider 或独立 Cache；
- P0 Metric Catalog 未冻结时 AC-3/动态 AC-7 保持 BLOCKED_EXTERNAL；
- native heartbeat mapping 未冻结时 target Health 验收保持 BLOCKED_EXTERNAL；
- AC-4 的逻辑 contract test 可在 MockProvider 上完整执行。

