# TDD-10：P0 Verification、Feature Specs Acceptance 与 Release Gate

> **TDD Version**：v0.8.2-tdd.1  
> **Baseline RFC**：PDCM Architecture Draft v0.8.2  
> **Status**：Proposed；AC-3 与真实 Provider/Heartbeat target tests 为 BLOCKED_EXTERNAL

## 1. Scope

本文冻结测试层级、版本化 Fixture、AC-1～AC-11 的当前 P0 解释、runtime loader 行为、单卡故障健壮性和发布门禁。AC-5、AC-6、AC-10 只验证 Not Supported，不建立正向能力或数值门槛。

## 2. Test Taxonomy

| Level | Environment | Purpose | Gate |
|---|---|---|---|
| Unit | no daemon/PDRL | 算法、状态机、边界、fake clock | every PR |
| Component | one component + fakes | ownership、queue、error contract | every PR |
| Contract | API/protocol/MockProvider | 公共语义和模块边界 | every merge |
| Integration-Mock | full daemon + deterministic MockProvider | 完整链路和可重复故障 | every merge/nightly |
| Integration-Target | FPGA/EMU + real PDRL | Discovery、Metric、heartbeat、recovery | release candidate |
| Robustness | mock/approved target lifecycle | crash/hang、timeout、capacity、invalid data | release candidate |
| Packaging/Docs | clean OS/image | install/service/CLI/docs | release candidate |

Performance/Soak 不作为 AC-10 或发布数值门禁。可以运行工程压力测试证明有界性，但报告必须标记 Engineering Robustness，不得宣称 AC-10 PASS。

## 3. Versioned Inputs

发布测试只能使用版本化 artifact：

- FPGA/EMU P0 Metric Catalog；
- Firmware Heartbeat Health Contract；
- Compatibility Matrix；
- Golden Inventory；
- Golden Metric/heartbeat source 和 tolerance；
- PDRL loader/ABI/error fixtures；
- platform restart/reload/reset/replug procedure；
- CLI/API/protocol golden schema；
- single-card support matrix。

当前状态：

| Artifact | 状态 |
|---|---|
| P0 Metric Catalog | BLOCKED_EXTERNAL |
| Provider ABI/loader contract | BLOCKED_EXTERNAL |
| heartbeat native mapping | BLOCKED_EXTERNAL |
| Generic API/protocol/Mock fixtures | 可实施 |
| AC-5/6/10 positive plans | 不需要，范围为 Not Supported |

测试报告记录 PDCM/PDRL/PDRV/PFW/HW version、target、host、fixture ID、Catalog hash、Compatibility Matrix version 和 seed。

## 4. Determinism

- Unit/component 使用 injected fake monotonic/wall clock，不依赖 sleep；
- MockProvider 按脚本返回结果，默认 seed 固定；
- target timing 只用于 deadline/freshness contract，不产生 AC-10 性能承诺；
- flaky retry 不能把失败结果转绿；
- 每个并发/故障 case 记录 request/session/watch ID、entity generation 和 Catalog generation。

## 5. Common Data Assertions

每个 Observation：

1. value type 与 Descriptor 一致；
2. unit 完全一致；
3. sample/observed time 合法；
4. status 只来自规定 enum；
5. generation/source/semantic version 可追溯；
6. invalid/unsupported/error 不含伪造 value；
7. partial failure 保留其他 item；
8. stale 包含 last-valid、age 和最新失败。

没有批准 tolerance 的动态 Metric 不能成为 Required。

## 6. AC-1：Install、Service、CLI、Compatibility

| ID | Procedure | Pass Criteria |
|---|---|---|
| AC1-001 | clean FPGA install | 文件、权限、unit 正确 |
| AC1-002 | clean EMU install | 同上 |
| AC1-003 | start/status/stop/restart | 有界完成且无残留 |
| AC1-004 | daemon absent 时 help/local version | exit 0，无 hang |
| AC1-005 | compatible CLI/daemon | handshake 成功 |
| AC1-006 | incompatible protocol major | Fail Fast，exit 7 |
| AC1-007 | invalid command/argument | exit 2，不创建资源 |
| AC1-008 | compatible upgrade | stop/replace/start/smoke |
| AC1-009 | incompatible package/runtime combination | 明确告警或拒绝，不假 Ready |
| AC1-010 | uninstall | 无进程、socket、active unit、owned binary |

PDRL 缺失时 daemon 应启动为 Degraded；该行为是 AC-1/Core lifecycle 的一部分，不表示真实 telemetry 可用。

## 7. Runtime Library Boundary

TDD-08 补齐后执行以下 target/packaging tests；系统级预期由 RFC 已冻结：

| ID | Scenario | Expected |
|---|---|---|
| LDR-001 | libpdrl.so 缺失 | daemon Degraded；Version=load failure；fresh request UNAVAILABLE |
| LDR-002 | 文件存在但不可装载 | daemon Degraded；failure phase LOAD |
| LDR-003 | bootstrap entry 缺失 | daemon Degraded；failure phase ENTRY |
| LDR-004 | ABI major 不支持 | daemon Degraded；failure phase ABI |
| LDR-005 | native initialize 失败 | daemon Degraded；failure phase NATIVE_INIT |
| LDR-006 | valid library | Provider 可进入 READY 并 Discovery |
| LDR-007 | shutdown/restart | 无悬空 callback/function pointer/handle |
| LDR-008 | binary dependency scan | PDCM 产物无 libpdrl DT_NEEDED |
| LDR-009 | untrusted caller path | API/CLI 无法覆盖为任意路径 |

精确 fixture、SONAME、symbol 和 fake library 需等待 TDD-08；未冻结前这些 target cases 为 BLOCKED_EXTERNAL。

## 8. AC-2：Single-card Discovery

### 8.1 Required Fields

单卡必须验证 Device count、PDCM ID、native ID、PCI BDF、state、PDRV version。Conditional 字段 supported 时与 Golden Inventory 一致；不支持时为 UNSUPPORTED。

### 8.2 Topology Cases

| Case | Expected |
|---|---|
| 0 Device | SUCCESS、count=0 |
| 1 valid Device | SUCCESS、count=1、字段正确 |
| 1 Device partial attributes | PARTIAL_RESULT，baseline/conditional 状态明确 |
| 2+ Device | UNSUPPORTED、detected count、entities empty、Core Degraded |
| malformed identity | item/descriptor error，不使用 index 修复 |
| reload/reorder | 单卡 ID 稳定；incarnation 变化推进 generation |

不得通过 --device 或 native ID 绕过多卡拒绝。

## 9. AC-3：Periodic P0 Metrics

状态：BLOCKED_EXTERNAL，解除条件为 P0 Metric Catalog 和 Provider mapping 冻结。

解阻后，对每个 target Required Metric 验证：

- 当前唯一 Device；
- single Metric 和 all Required Metrics；
- N=1 与 N>1；
- 每个周期恰有一个结果；
- type/unit/time/status/source/semantic version；
- 第 k 个样本失败仍计数；
- 一项失败不阻断其他 Metric；
- Watch 完成后只移除自身 owner；
- 默认周期和 min/freshness 与 Catalog 一致。

本 AC 不验证 jitter、sample loss ratio、p95、CPU/RSS 或持续运行时长。

在 Catalog 未冻结时，Mock Metric 测试只能证明框架能力，不能将 AC-3 标记 PASS。

## 10. AC-4：Firmware Heartbeat Health

P0 只测试 PDCM_HEALTH_SUBSYSTEM_FIRMWARE_HEARTBEAT：

| Evidence | Expected |
|---|---|
| fresh NORMAL | HEALTHY |
| fresh WARNING | WARNING |
| fresh FAULT | ERROR |
| missing | UNKNOWN |
| stale | UNKNOWN |
| timeout | UNKNOWN |
| read error | UNKNOWN |
| Provider unavailable | UNKNOWN + limitation |
| unsupported heartbeat | item UNSUPPORTED + UNKNOWN；target release gap |
| other subsystem request | item UNSUPPORTED |

还必须验证：

- result 包含 evidence、age、source、stable code、limitation；
- timeout 不归因为硬件 ERROR；
- newer normal heartbeat 可以恢复；
- duplicate/old generation evidence 不恢复；
- freshness timer 在没有新 commit 时转 UNKNOWN；
- Device aggregate 与 heartbeat 相同；
- Driver readiness/Device Access/PCIe/Memory/Sensor/RAS 不产生 P0 Health。

Mock contract tests可先执行；真实 target PASS 等待 heartbeat mapping 和 Golden Source。

## 11. AC-5：Basic Diagnostic

发布判定：Not Supported。

负向测试：

- operation_start/get/cancel 返回 UNSUPPORTED；
- 不创建 Operation ID、event、store 或 audit；
- Capability 不含 DIAG_BASIC；
- CLI 无 diag；
- package 无 pdcm-diag-runner；
- 无 Diagnostic Catalog；
- Provider 未收到 operation call。

测试成功只表示范围一致，不得写为 AC-5 PASS；报告状态为 NOT_SUPPORTED_BY_SCOPE。

## 12. AC-6：Fault Injection

发布判定：Not Supported。

验证：

- 公共 API/CLI 不提供 fault injection；
- package/documentation 不宣称注入、清除或归因能力；
- MockProvider timeout/malformed/item error 仅归入 AC-8 engineering robustness；
- 不要求 PCIe/Memory/Device Access 故障注入；
- 不生成 AC-6 fault manifest 或 positive qualification。

报告状态为 NOT_SUPPORTED_BY_SCOPE。

## 13. AC-7：Golden Source

### 13.1 Static

Discovery baseline 与 Golden Inventory 精确匹配；Conditional 字段按 supported/unsupported 验证。source 可追溯到 Provider。

### 13.2 Dynamic Metrics

等待 AC-3 解阻。对冻结 Required Metrics 按 Catalog/Test Plan 比对 value tolerance、unit、timestamp、freshness、status 和 sample count。

### 13.3 Heartbeat

验证 PDRL/平台 Golden heartbeat 分类与 PDCM HEALTHY/WARNING/ERROR/UNKNOWN 映射。Golden Source 不可用时为 BLOCKED_EXTERNAL，不允许人工观察后判 PASS。

## 14. AC-8：Single-card Robustness

场景：

- Device lost；
- 单 Metric read error；
- heartbeat timeout/read error；
- PDRL global timeout；
- missing/duplicate/oversize/malformed Provider result；
- malformed/oversize IPC；
- slow subscription；
- queue/cache/capacity exhaustion；
- Provider load/ABI/init failure。

Pass：

- daemon 不 crash/hang；
- 错误归属正确；
- 生命周期、Version 和可用的 Discovery 状态仍可查询；
- 单 Metric 失败时其他 Metric/heartbeat 按自身结果继续；
- heartbeat 失败时普通 Metric 不被自动停止；
- Cache 不被 malformed/old generation 数据污染；
- threads、FD、queues、Cache 保持配置上限。

不要求“其他 Device 继续”，因为 P0 不支持多卡。

## 15. AC-9：Restart 与外部生命周期恢复

### 15.1 Daemon Restart

活动 Watch/Health 下 restart：

- 旧 client 得到 disconnect/UNAVAILABLE；
- 新进程不把旧值标 VALID；
- client 重新 open/discovery/create Watch；
- baseline Watch 重建；
- 首个新样本后恢复有效结果。

### 15.2 Driver Reload

按平台批准流程：

- Provider 检测 unavailable/reload；
- 旧 data 为 STALE；
- Catalog/entity generation 变化；
- old EntityRef 返回 STALE_GENERATION；
- rediscovery 后新 Watch 和新样本建立。

### 15.3 External Reset/Replug

PDCM 不发起操作，只观察：

- lost/reappear；
- generation；
- stale-to-valid；
- heartbeat 从 UNKNOWN 恢复；
- 恢复前不发布旧 generation VALID。

若平台尚未提供批准流程，对应 target case 为 BLOCKED_EXTERNAL，不能改成可选跳过。

## 16. AC-10：Performance 与 Stability Feature

发布判定：Not Supported。

本版本不定义或门禁：

- 最大卡数；
- p95/p99 latency；
- sample loss ratio；
- period jitter；
- soak duration；
- CPU/RSS 数值；
- RSS growth per hour。

基础工程仍必须测试：

- 有界 queue/cache/thread/FD；
- request/provider/shutdown deadline；
- 过载时 RESOURCE_EXHAUSTED/loss marker；
- 无明显资源泄漏；
- 多客户端共享单卡采集。

这些结果归类为 Engineering Robustness，不得写 AC-10 PASS。

## 17. AC-11：Documentation 与 Release Consistency

CI 检查：

- 安装指南命令在 clean fixture 执行；
- CLI help 与 parser schema一致；
- single-card support matrix 与行为一致；
- P0 Metric Catalog 与 binary/catalog hash 一致；
- Health Contract 只有 Firmware heartbeat；
- public API reference 与 symbols/header 一致；
- stable errors 有说明；
- Compatibility Matrix 覆盖被测组合；
- Known Issues/Release Notes 版本一致；
- docs 不包含 diag、Fault Injection、AC-10 或多卡支持声明；
- no-DT_NEEDED 和 degraded loader 行为写入 Troubleshooting。

Metric/Provider 文档未冻结时 AC-11 不能 PASS。

## 18. API、ABI、Protocol Gate

- public symbol allowlist；
- ABI layout/version checker；
- old/new minor compatibility；
- protocol parser fuzz；
- protobuf unknown fields 和 frame fragmentation；
- no PDRL type/symbol in public headers；
- operation symbols 负向契约；
- Address/Undefined sanitizer；Thread sanitizer 在批准环境。

## 19. Security 与 Robustness Gate

- UDS credential/permission；
- invalid handle/struct_size/version/enum/buffer；
- untrusted provider path 输入被拒绝；
- malformed native values 和 strings 被长度限制/清理；
- no PDRL header outside provider directory；
- no public network listener；
- secrets 和未清理 native payload 不进入日志。

## 20. Traceability

| AC | Primary TDD | P0 判定 |
|---|---|---|
| AC-1 | 01、02、09 | Required |
| AC-2 | 03、08、09 | Required single-card；Provider target Blocked |
| AC-3 | 03、04、05、06、08 | Required；BLOCKED_EXTERNAL |
| AC-4 | 03、04、05、06、08 | Required heartbeat only；target Blocked |
| AC-5 | 02、07、09 | NOT_SUPPORTED_BY_SCOPE |
| AC-6 | 07、10 | NOT_SUPPORTED_BY_SCOPE |
| AC-7 | 03、05、06、08 | Required；dynamic/heartbeat target Blocked |
| AC-8 | 01、04、05、06、08 | Required single-card |
| AC-9 | 01、03、04、08 | Required；platform flow dependent |
| AC-10 | 00、10 | NOT_SUPPORTED_BY_SCOPE |
| AC-11 | 02、03、06、09 | Required |

每个 test metadata 引用至少一个 TDD requirement/acceptance ID 和一个适用 AC。

## 21. Release Gate Status

允许状态：

- PASS；
- FAIL；
- BLOCKED_EXTERNAL；
- NOT_SUPPORTED_BY_SCOPE；
- NOT_APPLICABLE_CONDITIONAL；
- WAIVED。

规则：

- SKIP 不是发布状态；
- Required AC 的 FAIL 或 BLOCKED_EXTERNAL 阻断发布；
- AC-5/6/10 必须为 NOT_SUPPORTED_BY_SCOPE，并通过负向范围检查；
- WAIVED 不得用于安全、ABI、单卡拓扑或 Required Health；
- Mock-only PASS 不能替代 target PASS。

## 22. Required Reports

每个 release candidate 输出：

- AC-1～AC-11 scope-aware summary；
- target/fixture/version/artifact hashes；
- Provider/TDD-08 dependency status；
- P0 Metric Catalog 和 coverage；
- Firmware heartbeat mapping/coverage；
- 0/1/>1 Device coverage；
- loader failure matrix；
- robustness/resource-bound summary；
- sanitizer/fuzz summary；
- known failures、blocked items、waivers；
- Not Supported feature negative-check result。

不输出 AC-10 性能 percentile 或“Basic Diagnostic passed”等超范围结论。

## 23. Completion Criteria

TDD-10 自身完成要求：

- 所有 Mock-level Required contract tests 可运行；
- AC-5/6/10 负向验证自动化；
- 0/1/>1 Device、heartbeat 四状态和 loader 降级矩阵有 case；
- 每项 release status 按范围生成；
- 无旧版多卡、Diagnostic、Fault Injection 或 performance gate 残留。

P0 Release Ready 还要求：

1. TDD-08 补齐并批准；
2. P0 Metric Catalog 和 heartbeat native mapping 冻结；
3. FPGA/EMU Golden Source 和 Compatibility Matrix 可用；
4. AC-1/2/3/4/7/8/9/11 无 FAIL/BLOCKED；
5. AC-5/6/10 正确标记 NOT_SUPPORTED_BY_SCOPE；
6. AC-11 文档与实际包一致。

