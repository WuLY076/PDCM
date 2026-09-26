# TDD-04：WatchManager 与 Collection Pipeline

> **TDD Version**：v0.8.2-tdd.1  
> **Baseline RFC**：PDCM Architecture Draft v0.8.2  
> **Status**：Proposed；具体 P0 Metrics 为 BLOCKED_EXTERNAL

## 1. Scope

本文冻结逻辑 Watch、共享 EffectiveWatch、采集计划、单卡调度、Fresh Read 合并、Provider 批量读取、结果规范化、重试边界、过载和 Catalog 变更。数据保存见 TDD-05，Metric/Health 处理见 TDD-06，原生 Provider 细节见待补齐的 TDD-08。

## 2. Core Principle

多个客户端和 internal processor 对同一 Device/Metric 的需求必须共享物理采集。删除一个 owner 只能移除该 owner requirement；只要其他 owner 仍存在，采集继续。

P0 只允许一个 Device generation 出现在 active plan。发现多卡配置时不生成任何 Device collection plan。

## 3. Terms

| Term | Meaning |
|---|---|
| Logical Watch | Session 或 internal processor 提交的采集需求 |
| Watch Owner | Session、HeartbeatHealth 或 system baseline |
| EffectiveWatch | 同一采集 key 的合并物理需求 |
| CollectionPlan | 在固定 Catalog generation 上的可执行分组 |
| CollectionJob | 某一 due time 的一次物理读取 |
| FreshRead | Query 显式要求立即获得满足 max_age 的结果 |

## 4. WatchRequirement

~~~cpp
struct WatchRequirement {
  WatchId id;
  OwnerId owner;
  uint64_t catalog_generation;
  EntityRef entity;
  std::vector<MetricId> metrics;
  Nanoseconds period;
  Nanoseconds freshness;
  Nanoseconds retention;
  uint64_t sample_limit;
  Priority priority;
  DeliveryMode delivery;
  bool allow_partial;
};
~~~

规则：

- Entity 必须是当前唯一受管 Device；
- selection 展开后不得超过 Session limit；
- period 必须满足每个 Metric 的 min/max；
- freshness >= period，retention >= freshness；
- unsupported Metric 返回 item UNSUPPORTED；
- 默认创建原子化，只有 allow_partial=true 才保留支持项；
- sample_limit 只终止 logical Watch，不直接停止共享 physical Watch。

## 5. WatchManager API

~~~cpp
class WatchManager {
 public:
  Result<WatchCreateResult> create(OwnerContext, WatchRequirement);
  Status destroy(OwnerContext, WatchId);
  Status removeOwner(OwnerId);
  void onCatalogChanged(const CatalogDiff&);
  WatchSnapshot snapshot() const;
};
~~~

WatchManager 不创建线程、不调用 Provider、不写 DataManager。

## 6. Merge Algorithm

P0 merge key：

~~~text
(provider_id, device.pdcm_id, device.generation, metric_or_evidence_id)
~~~

有效参数：

- period = 所有 owner period 的最小值；
- freshness = 所有 owner freshness 的最小值；
- retention = 所有 owner retention 的最大值；
- priority = 所有 owner priority 的最大值；
- logical sample counter 不合并；
- 若最终 Provider 声明某项不能共享，增加批准的 isolation class。

合并必须 deterministic。使用排序 key 和 copy-on-write：构建 next snapshot、校验成本/容量、原子替换、发送 PlanDelta。失败时保留 current snapshot。

## 7. Internal Baseline Watches

P0 internal owner 只有：

1. Firmware heartbeat Health evidence；
2. P0 Metric Catalog 解阻后标记 always_collect 的 Required Metrics。

不得为 Driver readiness、Device Access、PCIe、Memory、Sensor、RAS 或 Diagnostic 创建 P0 baseline Watch。

在 Metric Catalog 未冻结期间，只允许 heartbeat evidence 的 internal Watch；测试 Mock Metrics 不得进入生产 baseline。

## 8. Plan Compilation

CollectionCoordinator：

1. resolve 当前 EntityRef 和 descriptor；
2. 删除 stale generation、unsupported 或 blocked item；
3. 按 provider、period bucket 和 native batch compatibility 分组；
4. 按 Provider limits 切分 batch；
5. 计算 monotonic due time 和 deadline；
6. 生成 versioned immutable plan 并原子切换；
7. old plan 在途结果可完成，但提交时再次验证 generation。

requested period 不得静默 round 到更慢周期。scheduler coalesce window 可以合并接近的 due time，但每项仍保留 scheduled_time。

## 9. Scheduler

- 一个 scheduler thread 使用 monotonic clock；
- next_due 基于上一次计划 due time，不基于实际完成时刻；
- 同一 key 不并发执行；
- 错过周期时生成 miss accounting/NOT_AVAILABLE，不突发补采；
- scheduler 只向有界 provider queue 提交；
- queue full 时优先保留 heartbeat baseline，拒绝低优先级 Fresh Read 或普通 Watch；
- 优先级不能绕过全局硬上限。

## 10. Provider Read Request

~~~cpp
struct ProviderReadRequest {
  RequestId request_id;
  uint64_t plan_generation;
  uint64_t catalog_generation;
  MonotonicTime scheduled_time;
  MonotonicTime deadline;
  std::vector<ProviderReadItem> items;
};
~~~

每个 item 唯一对应当前 Device generation 和一个 Metric/heartbeat evidence key。Provider 返回调用级状态与可核对 key 的逐项结果。

缺项补为 ERROR_PROVIDER_MISSING_ITEM；重复/未知项视为 contract violation，不写入 Cache。

## 11. Result Normalization

~~~mermaid
sequenceDiagram
  participant W as WatchManager
  participant C as CollectionCoordinator
  participant P as ProviderManager
  participant D as DataManager
  W->>C: apply PlanDelta
  C->>P: batchRead with deadline
  P-->>C: call status and item results
  C->>C: validate plan, catalog, generation
  C->>D: commit normalized batch
  D-->>C: commit summary
~~~

CollectionCoordinator 必须：

- 生成 VALID/NOT_AVAILABLE/UNSUPPORTED/ERROR；
- 填充 scheduled/sample/observed time；
- 保留 provider/native source、native code 和 retryability；
- 不计算业务 derived metric 或 Health；
- 不将失败写成数值零；
- 拒绝 old generation 结果进入 current latest；
- 调用级失败时为所有请求项生成可解释结果。

## 12. Fresh Read

相同 key 的并发 Fresh Read 可以合并为一个 in-flight 请求，条件是：

- target generation、Catalog generation 相同；
- deadline 兼容；
- 读取 policy 和 provider isolation class 兼容。

行为：

- Cache 已满足 max_age 时不触发 Provider；
- in-flight 成功后所有 waiter 从 DataManager 读取提交结果；
- 一个 waiter timeout 不取消其他 waiter；
- provider queue 满返回 RESOURCE_EXHAUSTED；
- Provider UNAVAILABLE 返回 UNAVAILABLE，可按 allow_stale 附带旧值；
- Fresh Read 不永久创建 logical Watch。

## 13. Logical Sample Count

- 每个 scheduled period 产生一个 logical result token；
- VALID、NOT_AVAILABLE、UNSUPPORTED 和 ERROR 均计入 sample_limit；
- STALE 是 Query 表达，不作为新物理样本；
- logical Watch 达到 N 后自动销毁自身 requirement；
- 共享 physical Watch 只在没有其他 owner 时停止；
- AC-3 解阻后，dmon N 必须恰好输出 N 个逐周期结果。

## 14. Retry

- 只读 transient error 可以在原 deadline 内按最终 TDD-08 retryability 重试；
- 默认单次 job 不做无限重试；
- timeout worker 不得被 pthread_cancel；
- generation change、UNSUPPORTED、invalid result 不重试；
- P0 没有有副作用 Provider call；
- 重试不能产生额外 logical sample。

## 15. Catalog Change

- 新 snapshot 到达后生成 plan delta；
- old generation keys 立即停止新调度；
- removed Metric 向 owner 发布 UNSUPPORTED；
- changed period/freshness 重新编译；
- 多卡 topology unsupported 时清空 Device plan；
- Provider 恢复为单卡后按 owner requirements 和新 generation 重建；
- Metric Catalog 从 blocked 变为 frozen 必须通过正式 catalog version 变更。

## 16. Backpressure

硬上限包括：

- logical Watch 数；
- EffectiveWatch keys；
- plan groups/jobs；
- batch items；
- provider queue/in-flight；
- Fresh Read waiter；
- result batch bytes。

容量预检在发布 next snapshot 前完成。失败不得部分替换 current plan。

## 17. Thread Safety

- Watch registry 单 writer lock；
- plan snapshot immutable；
- scheduler 不持有 Watch registry lock；
- Provider call 和 DataManager commit 期间不持有 Watch/plan lock；
- owner removal、CatalogChanged 和 job completion 使用 generation token 消除竞态。

## 18. Failure Semantics

| Failure | 结果 |
|---|---|
| Provider global unavailable | 请求项 UNAVAILABLE/ERROR；旧值仅可 STALE |
| 单 Metric 失败 | 该项失败，其他 Metric 继续 |
| heartbeat 失败 | Health evidence 不确定；普通 Metrics 不被自动停止 |
| malformed provider item | 丢弃该项、contract error；其他合法项可提交 |
| old generation result | reject，不更新 current latest |
| queue full | RESOURCE_EXHAUSTED 或 periodic miss |
| 多卡发现 | 无 active Device plan，Core Degraded |

## 19. Tests

### 19.1 Merge 与 Ownership

- 多 Session 同 key 只产生一个物理采集；
- period/freshness/retention 合并；
- 删除一个 owner 不影响另一个；
- sample_limit 完成只删除自身；
- heartbeat internal owner 不随 Session 关闭。

### 19.2 Scheduling

- fake clock due order、no drift、miss、不并发同 key；
- queue full 和 priority；
- plan atomic swap；
- Catalog/generation change race。

### 19.3 Fresh Read

- cache hit；
- concurrent coalescing；
- waiter timeout isolation；
- Provider unavailable + stale；
- queue exhaustion。

### 19.4 Acceptance IDs

| ID | Requirement |
|---|---|
| COL-ACC-01 | 多客户端共享同一单卡底层采集 |
| COL-ACC-02 | N 次采样包含失败周期且恰好 N 个结果 |
| COL-ACC-03 | 单 Metric 失败不阻断其他 Metric |
| COL-ACC-04 | generation 改变后旧结果不进入 current Cache |
| COL-ACC-05 | Watch/queue/worker 始终有界 |
| COL-ACC-06 | 多卡配置不生成隐式第一卡采集计划 |

## 20. Completion Criteria

- Watch merge、scheduler、Fresh Read 和 Catalog change 使用 fake clock 全覆盖；
- 没有多卡 batch 承诺；
- internal baseline 只包含 heartbeat 和冻结后的 Required Metrics；
- Provider 原生假设不写入本模块；
- AC-3 未解阻前保持 BLOCKED_EXTERNAL；
- AC-3、AC-4、AC-8、AC-9 的相关 mock 验证可执行。

