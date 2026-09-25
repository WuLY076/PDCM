# TDD-05：DataManager、QueryEngine 与 SubscriptionManager

> **TDD Version**：v0.8.2-tdd.1  
> **Baseline RFC**：PDCM Architecture Draft v0.8.2  
> **Status**：Proposed for Implementation Review

## 1. Scope

本文冻结 Observation、DataManager store、batch commit、latest/history、freshness、QueryEngine、Event 和 Subscription 的 ownership、一致性、背压和回调投递。物理采集见 TDD-04，Metric/Heartbeat Health 见 TDD-06。

## 2. Ownership

DataManager 是 P0 telemetry、Event、Health result 和 evidence reference 的唯一长期内存 owner。其他模块可以持有不可变 snapshot/ref，不得维护第二份长期 Cache。

P0 不保存 Operation record、operation progress、diagnostic evidence 或 audit。

## 3. Observation

~~~cpp
struct Observation {
  EntityRef entity;
  MetricId metric_id;
  MetricValue value;
  ObservationStatus status;
  int64_t source_sample_time_ns;
  int64_t observed_monotonic_time_ns;
  int64_t observed_wall_time_ns;
  uint64_t catalog_generation;
  uint64_t commit_epoch;
  SourceMetadata source;
  ErrorMetadata error;
  DerivationMetadata derivation;
};
~~~

不变量：

- VALID 必须有类型正确的 value；
- ERROR、NOT_AVAILABLE、UNSUPPORTED 不得携带伪造 value；
- STALE 必须引用历史 last-valid value，并携带 age 和 latest failure；
- sample/observed time 不可用时必须显式说明；
- Entity generation、Metric semantic version 和 source 可追溯；
- string/bytes 值必须受 Catalog 和全局 byte limit 限制。

## 4. Store Layout

| Store | Key | 内容 |
|---|---|---|
| LatestResultStore | EntityRef + MetricId | 最近一次结果，包括失败 |
| LastValidStore | EntityRef + MetricId | 最近一次 VALID 值 |
| HistoryStore | EntityRef + MetricId | 有界 ring |
| HealthStore | EntityRef + heartbeat subsystem | 最近 HealthResult 和状态 |
| EvidenceStore | Evidence ID | Health 使用的有界证据引用 |
| EventStore | boot epoch + sequence | 有界事件窗口 |
| TombstoneStore | old EntityRef | generation/lost 摘要 |

Store 采用 shard，但一个 batch commit 具有单一 commit_epoch。P0 不要求跨进程持久化。

## 5. Locking

- shard 由稳定 hash 分配；
- 多 shard batch 按 shard ID 升序加锁；
- 发布 Event/processor notification 在释放 store lock 后执行；
- Query 读取 immutable copies，不持锁等待 Fresh Read；
- Subscription delivery 不持有 DataManager lock。

全局锁顺序遵循 TDD-00。

## 6. Batch Commit

提交步骤：

1. 验证 catalog/entity generation；
2. 验证 MetricDescriptor、type、unit 和 status/value；
3. 按 key 去重；重复项视为 contract error；
4. 分配 commit_epoch；
5. 更新 LatestResultStore；
6. VALID 时更新 LastValidStore 和 HistoryStore；
7. 失败结果不覆盖 LastValidStore，但记录为 latest；
8. 释放锁；
9. 发布 DataCommitted、MetricUpdate 或 error event。

old generation、未知 Metric、oversize value 和 malformed status 不得进入 current store。

## 7. Latest Semantics

- latest result：最近一次尝试的结果；
- last valid：最近一次成功值；
- Query 默认返回 latest result；
- latest 失败且 allow_stale=true 时，可返回 last-valid value，但 item status 强制为 STALE，并附 latest failure；
- 没有 last-valid 时不能返回 STALE；
- Provider unavailable 不会把旧值重新标为 VALID。

## 8. Retention

- History 同时受 max samples、max duration 和 global bytes 限制；
- 任一限制达到时淘汰最旧样本；
- Watch 删除后 key 进入 idle TTL；
- old generation 移至 TombstoneStore，不再接收写入；
- EvidenceStore 按 TTL 和引用状态淘汰；
- HealthStore 只保存 Firmware heartbeat；
- EventStore 不承诺跨 daemon replay。

## 9. Query Request

逻辑字段：

- selection；
- max_age_ns；
- read_policy；
- consistency；
- allow_stale；
- catalog_generation；
- result item/byte limit。

read_policy：

| Policy | 行为 |
|---|---|
| CACHE_ONLY | 只读 DataManager |
| CACHE_OR_FRESH | 满足 age 则返回，否则经 TDD-04 Fresh Read |
| FRESH_REQUIRED | 必须尝试 Fresh Read；失败时可按 allow_stale 附旧值 |

consistency：

- BEST_EFFORT：各 key 最新结果；
- AT_LEAST_EPOCH：等待到 deadline，要求 items 达到指定 epoch；
- SAME_BATCH：只接受同一 Provider batch；P0 不提供跨 batch snapshot。

## 10. Query Execution

~~~mermaid
sequenceDiagram
  participant Q as QueryEngine
  participant S as SemanticCatalog
  participant D as DataManager
  participant C as CollectionCoordinator
  Q->>S: resolve selection and capability
  Q->>D: read latest snapshot
  alt all items fresh enough
    D-->>Q: observations
  else policy permits fresh
    Q->>C: freshRead
    C-->>Q: completion
    Q->>D: read committed results
    D-->>Q: fresh, partial, or stale
  end
~~~

每个 requested pair 必须返回 item 或 result truncation 信息。排序固定为 entity kind、pdcm_id、generation、metric_id。

在单卡 P0 中，selection 展开后只允许当前 Device；多卡 topology unsupported 时 Query 不允许绕过 Discovery 直接指定某一 native ID。

## 11. Freshness

effective max age 为 caller 非零值与 MetricDescriptor freshness 的较小值；caller 为零时使用 descriptor 默认。age 使用 monotonic observed time。

Firmware heartbeat Health 使用独立的 Health freshness，普通 caller 不能放宽。source time 只有在明确同一 clock domain 时才可参与顺序判断。

## 12. Event Model

~~~cpp
struct PdcmEvent {
  uint64_t boot_epoch;
  uint64_t sequence;
  EventType type;
  Severity severity;
  std::optional<EntityRef> entity;
  int64_t occurrence_time_ns;
  uint64_t catalog_generation;
  Payload payload;
};
~~~

P0 event types：

- catalog/entity/capability changed；
- metric update；
- firmware heartbeat health changed；
- provider state changed；
- subscription loss marker；
- daemon draining。

Operation progress/completion 和 Diagnostic event 不属于 P0。

## 13. Subscription Filter

filter 可按：

- event types；
- 当前单卡 EntityRef；
- Metric IDs；
- Firmware heartbeat subsystem；
- minimum severity。

P0 不接受 operation ID 或其他 Health Subsystem filter。Catalog change 后，旧 generation 默认停止匹配；只有 follow_rediscovery=true 时，基于稳定 identity 重绑定当前唯一 Device。

## 14. Publish Pipeline

1. source module 构造 event draft；
2. EventStore 分配 boot_epoch/sequence；
3. SubscriptionManager 对 immutable subscription snapshot 匹配；
4. 为匹配订阅 enqueue 有界 event reference；
5. delivery worker 转为 IPC 或 embedded payload；
6. 成功交付更新 last delivered sequence。

CLI dmon 的逐周期输出依赖 Watch delivery token，而不是“值变化事件”。

## 15. Backpressure

每个 subscription 有 max events 和 max bytes：

- daemon draining 和 loss marker 为 non-droppable；无法 enqueue 时关闭 subscription；
- metric update 使用 DROP_OLDEST_SAME_KEY；
- health/entity/provider state change 使用 DROP_OLDEST_DROPPABLE；
- 首次丢弃插入一个 loss marker，后续拥塞更新同 marker；
- 慢消费者只消耗自身 quota，不阻塞 Data commit 或其他 Session。

收到 loss marker 后客户端必须重新 Query 当前状态。

## 16. Callback 与 IPC Delivery

- delivery 不持有 DataManager/Subscription registry lock；
- standalone 由 IPC output queue 写 socket；
- embedded 由 client callback executor 调用；
- close：mark closing → remove from matching snapshot → clear pending → wait active refcount → closed；
- close 返回后不再开始新 callback；
- malformed/oversize event 在序列化前拒绝并生成内部错误。

## 17. Derived Loop Prevention

Derived Observation provenance 包含 processor ID、input sequences、processor version 和 depth：

- processor 不能订阅自己的 output；
- dependency graph 必须无环；
- maximum derivation depth = 4；
- raw Provider Observation depth = 0；
- Catalog/entity generation 不一致时拒绝 derived commit。

## 18. Partial Result

| 场景 | Item / Call |
|---|---|
| key 从未采样 | NOT_AVAILABLE |
| capability absent | UNSUPPORTED |
| latest failed、允许 stale | STALE + last valid + latest error |
| latest failed、不允许 stale | ERROR，无 value |
| 一项失败 | batch PARTIAL_RESULT；保留其他项 |
| buffer too small | deterministic prefix + required count |
| Catalog 变化 | deadline 允许时重试一次，否则 STALE_GENERATION |
| Provider global unavailable | UNAVAILABLE；可选 stale attachment |

## 19. Tests

### 19.1 Store

- type/status/value/timestamp validation；
- latest result 与 last valid；
- ring/byte/TTL eviction；
- concurrent batch commit/read；
- generation rejection；
- capacity exhaustion。

### 19.2 Query

- 三种 read policy；
- Fresh Read coalescing 集成；
- deadline、partial、stale；
- deterministic order；
- SAME_BATCH/AT_LEAST_EPOCH；
- 多卡 topology 时不能绕过。

### 19.3 Subscription

- filter、sequence、overflow、loss marker；
- slow consumer isolation；
- close/callback race；
- daemon drain；
- catalog generation 和 follow_rediscovery。

### 19.4 Acceptance IDs

| ID | Requirement |
|---|---|
| DATA-ACC-01 | 五种 Observation 状态不混淆 |
| DATA-ACC-02 | 单项失败不丢失其他结果 |
| DATA-ACC-03 | STALE 包含 age、旧值和最新失败原因 |
| DATA-ACC-04 | 慢订阅者不阻塞采集或其他 Session |
| DATA-ACC-05 | 所有 store/queue 保持硬上限 |
| DATA-ACC-06 | Event 模型不包含 P0 Operation/Diagnostic |

## 20. Completion Criteria

- DataManager 是唯一数据 owner；
- Store/queue 同时具有 item 和 byte hard limit；
- Query/Subscription 的 generation、freshness 和 partial 行为明确；
- HealthStore 只允许 Firmware heartbeat；
- 不存在 Operation record/event/evidence；
- AC-3、AC-4、AC-7、AC-8、AC-9 的相关数据测试可执行，AC-3 target 部分保持外部阻塞。

