# TDD-08：ProviderManager、PdrlProvider 与 PDRL Compatibility

> **TDD Version**：v0.8.2-tdd.1  
> **Baseline RFC**：PDCM Architecture Draft v0.8.2  
> **Status**：Placeholder / BLOCKED_EXTERNAL  
> **Implementation Readiness**：Not Ready

## 1. 留白说明

PDRL 团队尚未最终冻结 P0 Metrics、共享库公共 ABI、Discovery/heartbeat 原生接口和生命周期语义。为避免把推测写成技术契约，本版不设计 Provider 类、加载算法、function table、函数映射、线程模型、timeout/retry 或错误映射。

本文件只记录 RFC v0.8.2 已经冻结、不能由后续 Provider 设计改变的系统边界，并列出补齐 TDD 所需输入。

## 2. RFC 已冻结的边界

以下内容不是本 TDD 的推测，而是 RFC 约束：

1. PDRL 以版本化共享库 libpdrl.so.<ABI-major> 交付。
2. PdrlProvider 是 PDCM 所有的内部静态组件，不是公共插件。
3. 只有 PdrlProvider 可以装载共享库、解析 PDRL ABI 或持有原生类型。
4. PDCM Core 只经 ProviderManager 使用 PdrlProvider。
5. libpdcm.so、pdcm-daemon 和其他 PDCM 产物不得形成对 libpdrl.so 的强制 DT_NEEDED。
6. PdrlProvider 必须在运行时加载并校验 PDRL ABI。
7. load、bootstrap entry、ABI compatibility、native initialization 任一步失败时 Provider 为 UNAVAILABLE，daemon 保持运行。
8. Provider UNAVAILABLE 时 Version/status 可用；PDRL-backed 请求返回 UNAVAILABLE；旧 Cache 只能显式 STALE。
9. P0 只支持 0 或 1 张 Device；多卡不得静默选择。
10. Firmware heartbeat 必须经现有 PdrlProvider 遥测通路提供。
11. P0 不实现任何 Provider Operation、Basic Diagnostic 或 Firmware Tool 旁路。
12. PDRL 原生 handle、struct、enum、symbol 和 error code 不得越过 Provider boundary。

## 3. 待 PDRL/平台冻结的输入

| 输入 | 当前状态 |
|---|---|
| 精确 SONAME 和安装位置 | TBD |
| bootstrap symbol 名称与签名 | TBD |
| API function table layout、struct_size、ABI major/minor 规则 | TBD |
| initialize/shutdown contract | TBD |
| thread safety、reentrancy、max concurrency | TBD |
| timeout、blocking、callback 和 cancellation 行为 | TBD |
| Device enumeration/identity/PCI BDF/status/PDRV 接口 | TBD |
| 0/1/>1 Device 的原生返回语义 | TBD |
| P0 Metric 列表、类型、单位、temporality、周期 | TBD |
| batch read 或逐项 read 能力 | TBD |
| Firmware heartbeat 数据结构、状态分类和 freshness 来源 | TBD |
| sample/source timestamp clock domain | TBD |
| Driver reload、reset/replug、device lost 通知/探测 | TBD |
| 原生错误码、retryability 和稳定错误映射 | TBD |
| FPGA/EMU/PDRV/PFW compatibility matrix | TBD |
| embedded 多实例/与业务进程共存支持 | TBD |

## 4. 后续必须补齐的章节

PDRL 输入冻结后，本文件必须新增并评审：

1. Ownership boundary 和源码边界；
2. Provider Contract v1 的精确类型；
3. ProviderManager/PdrlProvider 状态机；
4. 可信库查找、权限和加载顺序；
5. bootstrap/function table/ABI 协商；
6. initialize/shutdown 和异常清理；
7. call executor、worker、deadline 和 quarantine；
8. Discovery/identity/generation 映射；
9. P0 Metric 和 heartbeat 映射；
10. timestamp、unit、type 和 status 转换；
11. 原生错误表和 retry policy；
12. reload/lost/recovery；
13. MockProvider 与 contract tests；
14. Compatibility Matrix 和 target acceptance。

上述章节在本版故意不填写。

## 5. 禁止的临时假设

在本文补齐前，实现不得：

- 假设某个固定 bootstrap symbol；
- 假设 PDRL 所有 API thread-safe；
- 通过 Driver version 猜测 ABI；
- 将某个 native enum 直接作为 PDCM Metric ID；
- 用轮询频率猜测 heartbeat freshness；
- 将 timeout 自动判为硬件 FAULT；
- 搜索当前目录或普通用户可写路径加载共享库；
- 接受 API caller 指定任意共享库路径；
- 自动把逐项 API 包装成“已支持 batch”而不评估成本；
- 对未知执行状态的原生调用自动重试；
- 在 Provider 之外 include PDRL header。

## 6. 临时开发边界

在 TDD-08 解阻前：

- Core、Catalog、Watch、Data、Health 只能使用 deterministic MockProvider 开发；
- 真实 libpdrl.so adapter 不进入 release branch；
- AC-2 target、AC-3、AC-4 target、动态 AC-7、AC-8 target 和 AC-9 target 不能标记 PASS；
- Packaging 可以实现无 DT_NEEDED 检查和缺库降级框架，但可信路径/SONAME 的最终 golden 依赖本 TDD；
- 任何实验性适配必须标记 non-production，不能出现在公共 Capability。

## 7. Exit Criteria

本文件从 Placeholder 变为 Proposed 必须满足：

1. 第 3 节输入有书面版本化定义；
2. P0 Metric Catalog 已冻结；
3. Firmware heartbeat 原生 mapping 已冻结；
4. 兼容矩阵覆盖 FPGA 与 EMU 目标；
5. PDRL/PDCM 双方 Owner 已确认 ABI 和 lifecycle；
6. 第 4 节所有章节完成；
7. TDD-03、04、06、09、10 的引用同步更新。

