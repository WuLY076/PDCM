# TDD-07：Diagnostics 与 Operation（Post-P0 占位）

> **TDD Version**：v0.8.2-tdd.1  
> **Baseline RFC**：PDCM Architecture Draft v0.8.2  
> **Status**：Deferred / Not Applicable to P0

## 1. Purpose

本文不设计或实现 P0 Diagnostics/Operation。它只冻结当前版本的负向契约，防止旧版 TDD 中的 Basic Diagnostic、OperationManager、DiagnosticModule 和 pdcm-diag-runner 被误认为仍是交付内容。

## 2. P0 Decision

P0 不支持：

- AC-5 Basic Diagnostic；
- PDCM_OPERATION_DIAG_BASIC；
- Diagnostic Catalog；
- DiagnosticModule；
- ProviderOperationExecutor；
- pdcm-diag-runner；
- Firmware Tool 或 FirmwareToolProvider；
- 配置、reset、power cycle、隔离、节点驱逐；
- Operation progress/result/audit。

AC-6 Fault Injection 和 AC-10 性能验收也不通过 Operation 旁路实现。

## 3. Reserved Public ABI

以下符号保留：

- pdcm_operation_start；
- pdcm_operation_get；
- pdcm_operation_cancel。

P0 行为：

1. 校验 handle 和必要结构是否可安全读取；
2. 返回 PDCM_STATUS_UNSUPPORTED；
3. 不创建 operation ID；
4. 不调用 Provider；
5. 不预留 quota、worker、store、event 或 audit；
6. 不发布 Operation Capability；
7. 不根据请求类型返回不同的“半支持”结果。

out_operation_id 在返回 UNSUPPORTED 时不得被写成有效 ID。get/cancel 不将不存在的 ID 映射为 NOT_FOUND，因为当前版本整体能力不支持。

## 4. Build 与 Packaging

P0 构建和安装不得包含：

- pdcm-diag-runner；
- diagnostic catalog；
- diagnostic recommendation bundle；
- diag CLI command；
- operation worker 或 audit writer；
- Firmware Tool dependency。

允许保留内部空接口或源目录，用于保持未来扩展边界；它们不得启动线程或对外宣称能力。

## 5. Negative Tests

| ID | 验证 |
|---|---|
| OP-NEG-01 | operation_start 返回 UNSUPPORTED 且不写有效 ID |
| OP-NEG-02 | operation_get/cancel 返回 UNSUPPORTED |
| OP-NEG-03 | Capability 不含 Operation |
| OP-NEG-04 | CLI help 不含 diag/config/reset |
| OP-NEG-05 | package 不含 pdcm-diag-runner 和 Diagnostic Catalog |
| OP-NEG-06 | Provider mock 未收到 operation call |
| OP-NEG-07 | Data/Event/Audit 中没有 Operation 记录 |

这些测试是范围一致性验证，不表示 AC-5 已通过。

## 6. Future Enablement Gate

未来只有同时满足以下条件才能把本文改为 Active：

1. 新 RFC 明确 P0/P1 范围和数据来源；
2. Firmware/PDRL/PRT 中的权威执行方和接口已确认；
3. 安全、权限、副作用、取消、timeout 和审计契约已冻结；
4. Diagnostic Catalog 与稳定错误码已评审；
5. 是否需要独立 runner/process isolation 已完成风险评估；
6. CLI、Capability、Packaging 和 TDD-10 同步更新。

不得仅因为底层出现一个相似 API 就在实现中私自启用。

## 7. Completion Criteria

- P0 负向契约均由自动化测试覆盖；
- 旧版 Basic Diagnostic 设计不再被引用为 P0；
- 本文不进入 P0 正向功能开发计划；
- 状态保持 Deferred，直到新 RFC 正式启用。

