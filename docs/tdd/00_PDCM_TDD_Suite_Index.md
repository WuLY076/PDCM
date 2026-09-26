# PDCM P0 TDD 文档集总索引

## 1. 文档信息

| 项 | 值 |
|---|---|
| 基线 RFC | PDCM Architecture Draft v0.8.2 — Runtime Library Boundary and Scope Cleanup |
| TDD 集版本 | v0.8.2-tdd.1 |
| 状态 | Proposed；部分内容处于 BLOCKED_EXTERNAL |
| 硬件范围 | FPGA、EMU；仅单卡节点 |
| 生产数据源 | PDRL Public ABI，经内部 PdrlProvider 访问 |
| PDRL 交付物 | libpdrl.so.<ABI-major> |
| 公共动态库 | 仅 libpdcm.so |
| 生产模式 | standalone：libpdcm.so → local IPC → pdcm-daemon |
| 受限模式 | embedded：libpdcm.so → in-process PdcmServiceCore |

本文是 PDCM P0 TDD 文档集的规范入口。模块设计、实现、测试和发布评审必须同时遵循当前 RFC、本索引及对应模块 TDD；发生冲突时以 RFC 为最高优先级。

## 2. P0 范围冻结

| 范围 | P0 判定 |
|---|---|
| AC-1 安装、服务、CLI、版本兼容 | Required |
| AC-2 Discovery/Inventory | Required；仅 0 或 1 张可管理 Device |
| AC-3 P0 Metrics | Required，但具体 Metric Catalog 未冻结，当前 BLOCKED_EXTERNAL |
| AC-4 Health | Required；仅 Firmware heartbeat |
| AC-5 Basic Diagnostic | Not Supported |
| AC-6 Fault Injection | Not Supported |
| AC-7 权威数据比对 | Required；Metric 部分依赖 AC-3 解阻 |
| AC-8 故障健壮性 | Required；只验证单卡和单 Metric 语义 |
| AC-9 Restart/Reload/Reset/Replug 恢复 | Required；按平台批准的外部生命周期执行 |
| AC-10 性能与稳定性 Feature 验收 | Not Supported |
| AC-11 文档与兼容 | Required |

以下能力不属于 P0：多卡、Runtime/Program/Run/HLO、Basic Diagnostic、Fault Injection、配置写入、reset 命令、Policy、Profile、Stats、完整 Topology、Exporter、PRT Services 和 Firmware Tool。

## 3. 规范性语言

- MUST：P0 必须满足；违反即阻断对应代码合入或发布。
- MUST NOT：P0 禁止的行为。
- SHOULD：默认要求；偏离时必须给出评审记录和测试证据。
- MAY：可选实现，但不得改变公共契约。
- BLOCKED_EXTERNAL：设计目标已接受，但缺少 PDRL/平台冻结输入，不能宣称完成。
- Not Supported：当前版本不实现、不验收正向能力，只验证不会被错误暴露。
- Post-P0：仅保留演进边界，不属于当前构建、安装和发布能力。

## 4. 文档清单、状态与所有权

| TDD | 主题 | P0 状态 | 唯一负责的问题 |
|---|---|---|---|
| [TDD-01](01_Core_Lifecycle_and_Deployment_TDD.md) | Core Lifecycle 与 Deployment | Active | 进程/Core 生命周期、状态、启动关闭、单实例、恢复与部署 |
| [TDD-02](02_Public_API_IPC_and_Session_TDD.md) | Public API、IPC 与 Session | Active | C ABI、handle、双后端、本地协议、Session ownership |
| [TDD-03](03_Semantic_Catalog_Discovery_and_Capability_TDD.md) | SemanticCatalog、Discovery 与 Capability | Active / AC-3 Blocked | 单卡 Entity、generation、Catalog schema、Capability 和原子发布 |
| [TDD-04](04_Watch_and_Collection_Pipeline_TDD.md) | Watch 与 Collection Pipeline | Active / AC-3 Blocked | Watch 合并、调度、Fresh Read、批量采集与过载 |
| [TDD-05](05_Data_Query_and_Subscription_TDD.md) | Data、Query 与 Subscription | Active | Cache、查询一致性、Event、Subscription 与背压 |
| [TDD-06](06_Metrics_and_Health_TDD.md) | Metrics 与 Firmware Heartbeat Health | Active / AC-3 Blocked | Metric processor 通用规则和唯一 P0 Health 语义 |
| [TDD-07](07_Diagnostics_and_Operation_TDD.md) | Diagnostics 与 Operation | Deferred | 只冻结 P0 的 UNSUPPORTED 行为和未来启用条件 |
| [TDD-08](08_PDRL_Provider_and_Compatibility_TDD.md) | PDRL Provider 与 Compatibility | Placeholder / BLOCKED_EXTERNAL | 仅记录 RFC 已冻结边界；实现设计等待 PDRL 输入 |
| [TDD-09](09_CLI_Packaging_and_Release_TDD.md) | CLI、Packaging 与 Release | Active | P0 CLI、安装升级卸载、service、发布文档 |
| [TDD-10](10_Verification_and_Acceptance_TDD.md) | Verification 与 Acceptance | Active / 部分 Blocked | AC-1～AC-11 范围化验收和发布门禁 |

TDD-07 不产生 P0 实现工作包。TDD-08 当前不得被视为可实施设计。

## 5. 不可违反的架构约束

1. PDCM Core 只能通过 ProviderManager 使用底层能力。
2. 只有 PdrlProvider 可以装载 libpdrl.so、解析 PDRL 符号或持有 PDRL 原生类型、句柄和错误码。
3. PdrlProvider 是 PDCM 内部静态组件，不是公共插件。
4. PDCM 产物不得对 libpdrl.so 形成强制 DT_NEEDED 依赖。
5. libpdrl.so 缺失、不可装载、入口缺失、ABI 不兼容或原生初始化失败时，daemon 必须保持运行并进入可诊断的 Degraded。
6. P0 只管理每节点 0 或 1 张 Device；发现多于 1 张时不得选择第一张或返回部分受管清单。
7. P0 Health 只包含 PDCM_HEALTH_SUBSYSTEM_FIRMWARE_HEARTBEAT。
8. P0 不创建 Operation；所有 pdcm_operation_* 调用统一返回 PDCM_STATUS_UNSUPPORTED。
9. 普通 Query 只读取 DataManager；Fresh Read 必须显式经过 MetricsManager、CollectionCoordinator 和 ProviderManager。
10. MetricsManager 不持有第二份长期 Cache，不运行物理采集 timer，不直接调用 Provider。
11. 未支持项必须通过 Capability、item status 或调用状态明确表达，不能用零、空值或 SUCCESS 代替。
12. standalone 与 embedded 复用同一 PdcmServiceCore 和公共契约；两种模式不得形成不同业务语义。

## 6. 固定技术基线

| 领域 | 决策 |
|---|---|
| Public ABI | C11-compatible；opaque handles；可扩展结构包含 struct_size 和 version |
| Internal implementation | C++17；RAII；异常不得越过 C ABI、IPC 或 Provider boundary |
| Local IPC | Unix Domain Socket SOCK_STREAM；PDCM Local Protocol；protobuf payload |
| Time | duration 和 freshness 使用 monotonic clock；展示时间可使用 UTC wall clock |
| IDs | 显式稳定类型；不得将指针、原生 handle 或数组下标作为公共 ID |
| Data | Observation 的值、状态、时间、source 和 generation 不可分离 |
| Configuration | 系统只读配置和 daemon 参数；P0 无运行时写配置 API |
| Security | UDS permission + peer credential；无 TCP listener |
| Resource policy | 所有线程池、队列、Cache、Subscription 和请求大小均有硬上限 |

## 7. 建议源码边界

~~~text
include/pdcm/                 # 公共 C headers
src/common/                   # status、time、ID、logging、serialization
src/client/                   # libpdcm.so 与 BackendSelector
src/ipc/                      # Local Protocol transport/schema
src/core/                     # Core、Session、router、lifecycle
src/semantic/                 # Entity、Catalog、Capability
src/collection/               # Watch、scheduler、collection
src/data/                     # cache、query、subscription
src/metrics/                  # MetricsManager、processors、heartbeat health
src/operation/                # Post-P0 reserved；P0 不注册 executor
src/provider/                 # Provider Contract 与 ProviderManager
src/provider/pdrl/            # 唯一允许接触 PDRL 的目录
apps/daemon/
apps/cli/
proto/local/v1/
catalog/                      # schema；P0 Metric Catalog 待冻结
tests/unit/
tests/contract/
tests/integration/
tests/robustness/
config/
docs/release/
~~~

P0 构建不得产生 pdcm-diag-runner。构建检查必须拒绝 src/provider/pdrl/ 之外的 PDRL header 引用，并检查 libpdcm.so、pdcm-daemon 不含 libpdrl.so 的 DT_NEEDED。

## 8. 共享类型与唯一 Owner

| 类型 | Owner | 核心不变量 |
|---|---|---|
| EntityRef | TDD-03 | kind、pdcm_id、generation 唯一引用当前实体实例 |
| MetricDescriptor | TDD-03 | ID、类型、单位、scope、temporality 和 semantic version 在同一 Catalog generation 内不变 |
| Observation | TDD-05 | 一个 Entity/Metric 的一次结果；失败同样形成结果项 |
| CapabilitySet | TDD-03 | 只描述当前真实能力，并携带 Catalog generation |
| HealthResult | TDD-06 | 只表示 Firmware heartbeat，结论必须引用证据或限制 |
| ProviderResult | TDD-08 | 当前待冻结；最终必须分离调用级与逐项状态 |
| Operation | Post-P0 | P0 不创建、不保存、不发布 |

## 9. 错误与结果规则

1. API 返回值表达调用级结果，批量 item 另有独立状态。
2. 一部分成功、一部分失败时返回 PDCM_STATUS_PARTIAL_RESULT，并保留全部成功项。
3. Observation 状态固定为 VALID、STALE、NOT_AVAILABLE、UNSUPPORTED、ERROR。
4. STALE 必须包含旧值、sample time 和 age；从未有值时不能返回 STALE。
5. UNSUPPORTED 是稳定能力结论；NOT_AVAILABLE 是当前暂时没有数据。
6. ERROR 不得携带伪造数值；底层错误应保留 native source/code/retryability 元数据。
7. Provider 全局不可用时，PDRL-backed 请求返回 UNAVAILABLE；已有值只有显式允许 stale 时才可返回。
8. 其他 Health Subsystem 请求返回 item-level UNSUPPORTED；Heartbeat 证据不可确定时状态为 UNKNOWN。

## 10. 全局锁与调用边界

全局锁顺序：

1. Catalog writer/generation；
2. Session ownership；
3. Watch registry；
4. Collection plan；
5. Data shard；
6. Subscription item。

持有上述锁时 MUST NOT 调用 PDRL、等待 Provider future、写 IPC、调用用户 callback、写文件或执行外部日志 sink。跨层通信优先使用不可变快照和有界消息队列。

## 11. 统一资源边界

启动配置必须限定：

- Session、Watch、Subscription 和 outstanding request 数量；
- Entity/Metric selection 展开后项数；
- EffectiveWatch、batch size、Provider worker 和 quarantine 数量；
- 每 Metric 的历史样本数、时长和总 Cache bytes；
- Subscription events/bytes、IPC frame 和返回项数；
- request/fresh-read/shutdown deadline。

超过上限返回 RESOURCE_EXHAUSTED、BUFFER_TOO_SMALL 或按 TDD-05 生成 loss marker。P0 不通过 AC-10 承诺具体性能数值，但禁止无界资源。

## 12. 模块依赖

~~~mermaid
flowchart TB
  API["TDD-02 API / Session"] --> SEM["TDD-03 Semantic"]
  API --> MET["TDD-06 Metrics / Health"]
  MET --> COL["TDD-04 Watch / Collection"]
  MET --> DATA["TDD-05 Data / Query"]
  COL --> DATA
  COL --> PRV["TDD-08 Provider · TBD"]
  PRV --> SEM
~~~

TDD-07 不进入 P0 运行依赖。TDD-09 组装产物，TDD-10 验证所有已支持契约。

## 13. 外部冻结项

| 冻结项 | Owner | 解阻文档 |
|---|---|---|
| P0 Metric 列表、类型、单位、周期、freshness、目标支持 | PDRL/平台/PDCM 联合 | TDD-03、04、06、10 |
| libpdrl.so SONAME、bootstrap symbol、function table、ABI minor 规则 | PDRL/PDCM 联合 | TDD-08、09、10 |
| Discovery 与 heartbeat 原生结构和函数映射 | PDRL/PDCM 联合 | TDD-08 |
| 线程安全、timeout、reload、shutdown 语义 | PDRL/PDCM 联合 | TDD-08、01 |
| 原生错误码映射和 retryability | PDRL/PDCM 联合 | TDD-08、10 |
| FPGA/EMU Compatibility Matrix | PDRL/平台/PDCM 联合 | TDD-08、09、10 |

冻结前可以使用 MockProvider 开发 Core，但不能把 AC-3、真实 Health 或目标发布状态标记为 PASS。

## 14. 建议实施顺序

1. 公共状态、时间、Entity/Observation 类型和 MockProvider contract。
2. TDD-01 Core lifecycle 与 TDD-02 API/IPC/Session。
3. TDD-03 单卡 SemanticCatalog/Discovery。
4. TDD-05 DataManager/Query/Subscription。
5. TDD-04 Watch/Collection 与 Mock batch-read。
6. TDD-06 Firmware heartbeat Health 和通用 processor。
7. PDRL 输入冻结后补齐 TDD-08 和 P0 Metric Catalog。
8. TDD-09 CLI/Packaging。
9. TDD-10 target verification 和 release gate。

## 15. Pull Request Definition of Done

每个模块 PR 必须：

- 引用 TDD requirement/test ID；
- 提供成功、错误、timeout、capacity 和 shutdown 单测；
- 不新增无界线程、队列、缓存或自动重试；
- 不泄漏 PDRL 类型到 Provider boundary 以上；
- 不引入多卡、Diagnostic、Fault Injection 或 AC-10 支持声明；
- 对公共/内部接口说明 ownership、thread-safety、deadline 和 error contract；
- 通过 standalone/embedded 共用契约测试；
- 行为改变时同步修改 TDD 和验证矩阵。

## 16. 文档集完成标准

以下条件全部满足后，本 TDD 集才可标记 Implementable：

1. TDD-01～06、09、10 评审通过；
2. TDD-07 保持 Deferred，负向契约已测试；
3. TDD-08 从 Placeholder 补齐并评审通过；
4. P0 Metric Catalog 与 Firmware heartbeat 原生契约冻结；
5. FPGA/EMU Compatibility Matrix 和 Golden Source 可用；
6. AC-1/2/3/4/7/8/9/11 均可追溯到实现和测试。
