# TDD-01：PDCM Core Lifecycle 与 Deployment

> **TDD Version**：v0.8.2-tdd.1  
> **Baseline RFC**：PDCM Architecture Draft v0.8.2  
> **Status**：Proposed for Implementation Review

## 1. Scope

本文冻结 pdcm-daemon、PdcmServiceCore、组件装配、进程状态、启动与关闭顺序、线程域、单实例、readiness/liveness、降级和恢复。公共 ABI 与 IPC 见 TDD-02；PDRL 加载和原生生命周期细节待 TDD-08 补齐。

## 2. Goals

- daemon 在 libpdrl.so 或 PDRL 不可用时仍能启动、接受基础请求并报告明确原因；
- 0 或 1 张 Device 是受支持拓扑，多于 1 张时不管理任何 Device；
- 所有组件按确定顺序创建、发布、停止和销毁；
- daemon restart、Driver reload、外部 reset/replug 后重新发现并推进 generation；
- 旧 Cache 在恢复前只能以 STALE 返回；
- standalone 与 embedded 复用同一 Core；
- 所有 shutdown、队列和等待都有上限。

## 3. Non-Goals

- 不实现 PRT Services 或网络监听；
- 不实现 Operation、DiagnosticModule 或 pdcm-diag-runner；
- 不实现 PDCM 发起的 reset、power cycle 或配置写入；
- 不承诺 embedded 与业务 Runtime/PDRL 共存，除非后续兼容矩阵批准；
- 不在本文冻结 PDRL bootstrap、function table、原生函数和 Metric 映射。

## 4. Runtime Components

| 组件 | 生命周期 | 失败策略 |
|---|---|---|
| RuntimeConfig | process/Core lifetime | 配置结构或安全校验失败为 Fatal |
| StructuredLogger | process lifetime | 初始化失败写 stderr 并退出 |
| SemanticCatalog | Core lifetime | 内建 schema 损坏为 Fatal |
| DataManager | Core lifetime | 必需 store 无法建立为 Fatal |
| ProviderManager | Core lifetime | 可为 UNAVAILABLE，Core 进入 Degraded |
| WatchManager | Core lifetime | Registry 无法建立为 Fatal |
| CollectionCoordinator | Ready/Degraded | 失败停止新采集并进入 Degraded |
| MetricsManager | Core lifetime | processor 初始化失败按 Required/Conditional 分类 |
| SessionManager | Accepting lifetime | 停止后拒绝新 Session |
| PdcmApiService | daemon listener lifetime | bind/permission 失败为 Fatal |

P0 不实例化可执行 OperationManager。若代码保留空边界，也不得创建 worker、store、audit 或 Capability。

## 5. Core State Machine

~~~mermaid
stateDiagram-v2
  [*] --> Created
  Created --> Starting: start
  Starting --> Ready: core ready and provider ready
  Starting --> Degraded: provider unavailable or topology unsupported
  Starting --> Failed: core invariant failure
  Ready --> Degraded: provider or collection failure
  Degraded --> Ready: compatible provider and supported discovery recovered
  Ready --> Stopping: stop
  Degraded --> Stopping: stop
  Failed --> Stopping: cleanup
  Stopping --> Stopped: bounded join complete
  Stopped --> [*]
~~~

状态定义：

- Ready：PDCM 必需组件可用，Provider READY，Discovery 已完成且设备数为 0 或 1。
- Degraded：API 仍可提供 Version、Core status、错误诊断和允许的 stale 数据；Provider-backed fresh 请求返回明确失败。
- Failed：PDCM 自身不变量无法建立，例如内建 schema 损坏、DataManager 必需资源失败。
- Stopping：拒绝新 Session、Watch、Subscription 和 Fresh Read，只执行排空与清理。

0 张 Device 不等于 Provider 失败；Core 可以 Ready，但 Device Capability 为空。多于 1 张 Device 必须 Degraded，且不得发布任意一张为受管 Device。

## 6. Startup Sequence

~~~mermaid
sequenceDiagram
  participant M as daemon main
  participant C as PdcmServiceCore
  participant S as SemanticCatalog
  participant D as DataManager
  participant P as ProviderManager
  participant A as PdcmApiService
  M->>M: validate config and acquire process lock
  M->>C: construct
  C->>S: load built-in schemas and target catalog
  C->>D: initialize bounded stores
  C->>P: initialize production provider
  P-->>C: READY or UNAVAILABLE with reason
  C->>S: commit discovery outcome
  C->>C: start collection and processor domains
  C-->>M: Ready or Degraded
  M->>A: bind UDS and accept
~~~

强制步骤：

1. 校验配置、路径、容量和模式；不得由普通客户端覆盖 Provider 共享库路径。
2. 建立日志、clock、boot epoch、process lock。
3. 加载内建 schema、Health Catalog 和可用的 target catalog。
4. 初始化空 DataManager；不得加载上次进程的 telemetry 作为新鲜数据。
5. ProviderManager 初始化 PdrlProvider。已知失败原因至少区分：library load、bootstrap entry、ABI compatibility、native initialize。
6. Provider READY 时执行 Discovery；0/1/>1 分别按 TDD-03 处理。
7. 构建 processor graph 和 internal baseline Watch。AC-3 Catalog 未冻结时不得注册虚构 Required Metrics。
8. 启动 scheduler、provider worker、processor worker 和 delivery worker。
9. Core 达到 Ready 或 Degraded 后才绑定 UDS。

libpdrl.so 的具体加载顺序和符号检查不在本 TDD 冻结。

## 7. Shutdown Sequence

1. 原子进入 Stopping，listener 停止接受新连接。
2. SessionManager 将活动 Session 转为 Draining，拒绝新资源。
3. SubscriptionManager 停止匹配并排空/关闭投递。
4. WatchManager 删除 owner requirements；CollectionCoordinator 停止新调度。
5. 等待 request、collection 和 processor 工作至 shutdown grace。
6. ProviderManager 停止接收新调用，等待或隔离在途调用。
7. 调用 Provider shutdown；完成后不得再访问 PDRL function table 或共享库句柄。
8. DataManager 丢弃进程内 telemetry；P0 不要求持久化。
9. 逆创建顺序 join 全部线程。
10. 删除 UDS、pid/lock 文件并退出。

stop 必须幂等。析构函数不得发起无限等待或新的 PDRL 调用。

## 8. Process Ownership

- 每节点最多一个 standalone pdcm-daemon，通过受信任 runtime 目录下的 flock 保证；
- 默认 UDS 为 /run/pdcm/pdcm.sock，父目录不得普通用户可写；
- stale socket 只有持有 process lock 的 daemon 可以清理；
- embedded Core 不创建 UDS、不使用 daemon lock、不共享 daemon Cache；
- embedded 是否能创建 PDRL owner 由最终 TDD-08 和 Compatibility Matrix 决定；未确认时仅允许 MockProvider。

## 9. Thread Model

| Thread/Pool | 数量 | 允许阻塞 | 任务 |
|---|---:|---|---|
| main/signal | 1 | 否 | 生命周期和信号 |
| IPC reactor | 1 | 短时 | accept/read/write readiness |
| request workers | bounded | 只允许有 deadline 的等待 | routing 和序列化 |
| collection scheduler | 1 | 否 | timer、plan、dispatch |
| provider workers | bounded；保守默认 1 | 是，必须有 deadline/watchdog | Provider/PDRL 调用 |
| processor workers | bounded | 否 | derived metric 和 heartbeat health |
| delivery workers | bounded | 短时 | IPC/callback enqueue |

禁止 detached thread。队列满时返回 RESOURCE_EXHAUSTED 或产生 TDD-05 定义的 loss marker，不能动态创建线程扩容。

## 10. Runtime Configuration

配置优先级为 compiled defaults < /etc/pdcm/pdcm.conf < daemon command line。生产默认 strict：未知键阻止启动。

| 分类 | 必需约束 |
|---|---|
| IPC | absolute socket path、max frame、request deadline |
| Capacity | sessions、watches、subscriptions、cache bytes、queue depths |
| Collection | min/max period、batch size、scheduler coalesce window |
| Provider | 只能来自管理员配置；具体可信路径与优先级待 TDD-08/09 冻结 |
| Recovery | retry backoff、watchdog、shutdown grace |
| Logging | level、sink、rotation；不得泄漏未经清理的 native payload |

配置热更新不属于 P0。任何变更需要 daemon restart。

## 11. Readiness 与 Liveness

- Liveness 只表示进程事件循环和核心线程未死锁，不依赖 Device 或 PDRL 可用。
- Readiness 返回 Core state、Provider state、discovery status、detected device count、catalog generation 和 degraded reason。
- Provider UNAVAILABLE 时进程可 live，服务状态为 Degraded。
- 发现多卡时进程可 live，服务状态为 Degraded/topology unsupported。
- 0 张 Device 且 Provider 正常完成枚举时可 Ready，Device count 为 0。
- Health 不能替代 service readiness；heartbeat UNKNOWN 不自动使 daemon liveness 失败。

## 12. Recovery

### 12.1 Daemon restart

- telemetry Cache 不跨进程恢复；
- 旧 standalone handle 在断连后返回 UNAVAILABLE；
- 客户端必须重新 open 并创建 Watch/Subscription；
- 新进程在第一批有效数据前返回 NOT_AVAILABLE，不伪造 STALE；
- internal baseline Watch 在 Discovery/Catalog 完成后重建。

### 12.2 Provider/Driver recovery

1. ProviderManager 转为 UNAVAILABLE，停止新调用。
2. DataManager 将相关最新有效值保留为可选 STALE。
3. 使用有界退避尝试重新初始化和 Discovery；具体原生重试条件待 TDD-08。
4. SemanticCatalog 原子提交新 catalog/entity generation。
5. WatchManager retire 旧 generation 计划并按新 Capability 重编译。
6. 第一批成功采样后才能将对应 Observation 标为 VALID。
7. Provider、Discovery 和 baseline requirements 全部满足后 Core 可回到 Ready。

Device lost、Driver reload、外部 reset/replug 均遵循同一 generation 规则。PDCM 不主动执行这些操作。

## 13. Signal 与退出码

| 场景 | 行为 | Exit code |
|---|---|---:|
| SIGTERM/SIGINT clean stop | 有界优雅关闭 | 0 |
| PDCM 配置或内建 schema fatal | listener 不启动并清理 | 2 |
| UDS bind/permission failure | 启动失败 | 3 |
| second instance | 不修改现有实例 | 4 |
| PDCM invariant failure | 记录 fatal 并有界清理 | 5 |
| libpdrl.so 缺失/入口缺失/ABI 不兼容/init 失败 | 保持 Degraded | 不退出 |
| 0 Device | 正常服务，清单为空 | 不退出 |
| 多于 1 Device | 保持 Degraded/topology unsupported | 不退出 |

PDRL ABI 不兼容不得归类为 PDCM Core ABI fatal。

## 14. Observability

每次状态变化记录 old_state、new_state、reason、monotonic timestamp 和 correlation ID。内部自监控至少包含：

- core/provider state transitions；
- active sessions/watches/subscriptions；
- queue depth/dropped；
- provider calls/timeouts/quarantined workers；
- collection batches/partial results；
- cache bytes/observations；
- rediscovery attempts/outcomes；
- detected device count 和 topology unsupported 次数。

P0 不要求 exporter。

## 15. Failure Matrix

| Failure | Core state | 外部行为 | 恢复 |
|---|---|---|---|
| Built-in schema invalid | Failed | listener 不启动 | 修复发布包 |
| PDRL library/load/entry/ABI/init failure | Degraded | version/status 可用；PDRL-backed 请求 UNAVAILABLE | 安装/升级依赖后 restart 或批准的 reinit |
| 0 Device | Ready | discovery SUCCESS + 0 | 后续 rediscovery |
| 多于 1 Device | Degraded | discovery UNSUPPORTED + detected count | 变为受支持单卡后 rediscovery |
| 单 Metric 读取失败 | Ready 或带 warning | 该项失败，其他项继续 | 下一周期或显式 fresh |
| Device lost | Degraded | 旧数据仅 STALE；Discovery 反映 lost/0 | reappear 后新 generation |
| collection queue full | Ready with warning | 新 Fresh Read 可 RESOURCE_EXHAUSTED | drain/backpressure |
| Data capacity exceeded | Degraded 或 warning | 按策略淘汰并报告 | 调整批准配置 |

## 16. Tests

### 16.1 Unit

- 状态转换和非法转换全覆盖；
- startup 每一步故障注入，验证逆序释放；
- stop 幂等、并发 stop、Starting 中 stop；
- 配置优先级、未知键、路径与容量；
- process lock、stale socket、second instance；
- 0/1/>1 Device 的状态判定。

### 16.2 Integration

- MockProvider UNAVAILABLE 时 daemon Degraded，但 Version/status 可用；
- loader 四类失败原因能够透出且进程保持运行；
- Provider 恢复后 generation、Catalog 和 Watch 重建；
- active query/watch/subscription 下 SIGTERM 有界退出；
- 100 次 start/stop 无 thread、FD、socket 或 lock 泄漏；
- standalone/embedded 在 MockProvider 下返回等价业务结果。

### 16.3 Acceptance IDs

| ID | Requirement |
|---|---|
| CORE-ACC-01 | daemon 可 Start/Stop/Restart，卸载前可完整停止 |
| CORE-ACC-02 | PDRL load/ABI/init 失败不阻止 daemon 启动 |
| CORE-ACC-03 | 0/1/>1 Device 的 Core 状态与 Discovery 语义正确 |
| CORE-ACC-04 | reload/reset/replug 后旧 generation 不再产生 VALID |
| CORE-ACC-05 | 单 Metric/timeout 不导致 daemon crash/hang |
| CORE-ACC-06 | shutdown 在批准 grace 内完成且无残留线程/进程 |

## 17. Completion Criteria

- 状态机、启动关闭顺序和失败矩阵均有自动化测试；
- 没有 Operation/Diagnostic worker 或 runner；
- 没有 detached thread、无界 queue 或析构无限等待；
- standalone 与 embedded 共用 PdcmServiceCore factory；
- PDRL header/type 未进入本模块；
- AC-1、AC-2、AC-8、AC-9 的相关验证可由 TDD-10 执行。

