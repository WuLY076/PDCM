# TDD-09：CLI、Packaging、Service 与 Release Documentation

> **TDD Version**：v0.8.2-tdd.1  
> **Baseline RFC**：PDCM Architecture Draft v0.8.2  
> **Status**：Proposed；PDRL 路径/SONAME细节等待 TDD-08

## 1. Scope

本文冻结 pdcm-cli 的 P0 命令、退出码、human/JSON 输出、help、文件布局、service、安装升级卸载、链接检查和发布文档。业务语义由 TDD-02～06 定义，PDRL 具体安装位置和 ABI 兼容项待 TDD-08。

## 2. CLI Principles

- CLI 只使用 libpdcm.so 公共 API，不直连 daemon 内部接口或 PDRL；
- stdout 只输出命令结果，stderr 输出诊断和日志；
- human 与 JSON 使用同一 domain result；
- partial/unsupported/unavailable 不能被格式化层改成成功；
- 默认只连接 standalone daemon，不隐式启动 embedded；
- P0 命令只面向当前唯一 Device，不提供多卡 --all 语义；
- help/version 的本地部分不依赖 daemon；
- Post-P0 命令不出现在 P0 help。

## 3. P0 Command Surface

| Command | 作用 | Daemon |
|---|---|---|
| pdcm-cli help / --help | 命令帮助 | 不需要 |
| pdcm-cli version | 本地、daemon、Provider、PDRL 状态 | 本地部分不需要；完整信息需要 |
| pdcm-cli discovery | 单卡 Discovery/Inventory | 需要 |
| pdcm-cli dmon | P0 Metrics 周期输出 | 需要；Catalog 未冻结时 Blocked |
| pdcm-cli health | Firmware heartbeat Health | 需要 |

P0 不包含：diag、config、reset、group、policy、profile、stats、topology、modules、introspect、exporter。

## 4. Global Options

| Option | 规则 |
|---|---|
| --format human/json | 默认 human |
| --timeout DURATION | 正值且不超过编译安全上限 |
| --endpoint PATH | 仅允许绝对 UDS path；生产 policy 可禁止覆盖 |
| --mode standalone/embedded | 生产 CLI 默认且推荐 standalone |
| --verbose | 只改变 stderr 诊断，不改变结果和退出码 |
| --device ID | 可选；若给出必须等于当前唯一 Device ID |

P0 不提供 --all。未给 --device 时，命令使用 Discovery 返回的唯一 Device；0 Device 返回明确无设备结果；多卡返回 unsupported configuration。

## 5. Exit Codes

| Exit | Meaning |
|---:|---|
| 0 | 完全成功 |
| 1 | 命令完成但有 partial item failure |
| 2 | usage/argument error |
| 3 | unsupported capability、unsupported topology 或 catalog blocked |
| 4 | permission denied |
| 5 | daemon/Provider/PDRL/Device unavailable |
| 6 | timeout/resource exhausted |
| 7 | CLI/library/daemon protocol major incompatible |
| 8 | internal/serialization error |

Help 和 local version 成功时 exit 0。JSON 模式仍使用相同 exit code。

## 6. Common JSON Envelope

~~~json
{
  "schema_version": 1,
  "command": "health",
  "status": "SUCCESS",
  "request_id": 1,
  "catalog_generation": 2,
  "core_state": "READY",
  "provider_state": "READY",
  "items": [],
  "errors": []
}
~~~

规则：

- key 和 enum 使用稳定英文标识；
- 人类解释可本地化，但 JSON schema 不本地化；
- int64 时间/计数不经过浮点；
- value 缺失时不输出伪造 0；
- STALE 同时输出 sample_time、age 和 latest_error；
- partial 必须列出成功与失败 item；
- 任何 native message 必须清理且有长度上限。

## 7. version

输出：

- CLI/libpdcm/public ABI/local protocol；
- daemon/Core/protocol；
- Provider state；
- PDRL load、compatibility 和 failure phase；
- PDRL/PDRV version，若可得；
- target、Catalog version/generation；
- 单卡支持范围。

Provider failure phase 至少显示 stable 分类：load、entry、ABI、native-init、discovery。不得把任意 dlerror/native 文本直接放入 JSON。

daemon 不存在时：

- 输出 local components；
- daemon/provider fields 标记 UNAVAILABLE；
- 命令是否非零由是否显式要求 daemon 决定；默认完整 version 返回 partial exit 1。

## 8. discovery

输出 Device count 和每个 Device 的 baseline/conditional 字段：

- 0 Device：SUCCESS、count=0；
- 1 Device：输出 PDCM ID、native ID、PCI BDF、state、PDRV；
- >1 Device：exit 3，status UNSUPPORTED，输出 detected_device_count，不输出第一张 Device；
- conditional field 不支持：字段 item status UNSUPPORTED；
- Provider unavailable：exit 5。

## 9. dmon

参数：

- --metric NAME_OR_ID，可重复；
- --count N，N > 0；
- --period DURATION；
- --fresh 或默认 shared Watch；
- --format。

输出每个逻辑周期的 timestamp、Device、Metric、value/status、unit、source 和 age。失败周期计入 N。

P0 Metric Catalog 未冻结时：

- help 可以展示通用参数，但不得列出虚构 Metric；
- 未指定可用 Metric 或请求 production P0 set 时，返回 CATALOG_BLOCKED_EXTERNAL/exit 3；
- release candidate 不能以该状态通过 AC-3。

Catalog 冻结后，help/listing 必须从同一 Catalog 生成。

## 10. health

P0 仅允许：

~~~text
pdcm-cli health [--device ID] [--fresh]
~~~

默认并唯一 subsystem 为 firmware_heartbeat。输出 state、item status、evidence age、stable code、source 和 limitations。

- 请求其他 subsystem：exit 3/UNSUPPORTED；
- evidence stale/missing/timeout：命令可完成，但 Health state=UNKNOWN；
- explicit WARNING/ERROR：domain status 可 SUCCESS，CLI exit policy固定为 WARNING=1、ERROR=1；
- Provider unavailable：exit 5，若有旧 evidence则附 STALE。

## 11. Help Contract

- pdcm-cli help、--help、COMMAND --help 不需要 daemon；
- 文本从 command schema 生成；
- option 显示类型、默认、范围和示例；
- dmon Metric 名称从冻结 Catalog 生成；
- diag 和其他 Post-P0 命令不得出现；
- 单卡限制和多卡 unsupported 必须在 discovery/dmon/health help 中说明。

## 12. Filesystem Layout

| Path | Content | Ownership/Mode |
|---|---|---|
| /usr/lib*/libpdcm.so.<major> | public library | root:root 0755 |
| /usr/include/pdcm/*.h | public headers | root:root 0644 |
| /usr/bin/pdcm-cli | CLI | root:root 0755 |
| /usr/sbin/pdcm-daemon | daemon | root:root 0755 |
| /etc/pdcm/pdcm.conf | runtime config | root:root 0640 |
| /run/pdcm/pdcm.sock | runtime UDS | service user:pdcm 0660 |
| /usr/share/pdcm/catalog/ | approved P0 catalog resource | root:root 0644 |
| /usr/share/doc/pdcm/ | release docs | root:root 0644 |

P0 不安装 /usr/libexec/pdcm/pdcm-diag-runner。

libpdrl.so 由 PDRL 团队交付，不是 PDCM package-owned 文件。精确位置、SONAME 和权限规则待 TDD-08/Compatibility Matrix 冻结；PDCM package 不复制、改名或生成兼容 symlink。

## 13. Link 与 Runtime Dependency

构建/发布检查：

- readelf/objdump 检查 libpdcm.so、pdcm-daemon、pdcm-cli，不得出现 libpdrl.so 的 DT_NEEDED；
- PDRL headers 不安装到 PDCM public include；
- libpdrl.so 缺失时 package install 可以成功，daemon 启动为 Degraded；
- 入口缺失、ABI 不兼容、init 失败时 daemon 不崩溃；
- 普通用户不能通过 CLI/API 指定任意 Provider library path；
- 最终可信路径和权限 golden 在 TDD-08 解阻后补齐。

## 14. Service Definition

systemd 或平台等价配置必须：

- RuntimeDirectory=pdcm；
- 使用专用 user/group 或批准的最小权限；
- 创建受信任 UDS；
- crash 可按 policy restart，正常 stop 不自动重启；
- TimeoutStartSec/TimeoutStopSec 覆盖 TDD-01 有界 lifecycle；
- 不需要网络 capability；
- 文件系统和 device access 使用 allowlist；
- ExecStartPre 只校验 PDCM config/catalog，不装载或修改设备；
- PDRL 不可用时不让 service manager 反复 crash-loop。

## 15. Package Split

建议：

- pdcm-runtime：daemon、libpdcm、CLI、service、config、catalog；
- pdcm-devel：public headers、linker symlink、pkg-config/CMake metadata；
- pdcm-doc：发布文档，可独立包但必须同版本发布。

不发布内部静态库，不打包 runner，不打包 libpdrl.so。

## 16. Install、Upgrade、Uninstall

### 16.1 Install

安装文件 → 创建 user/group/runtime dirs → 安装 unit → daemon-reload。是否自动 start 由平台 policy 冻结。PDRL 缺失不得导致文件安装事务回滚，但 smoke test 必须报告 Degraded。

### 16.2 Upgrade

1. 校验 PDCM package 自身兼容；
2. 若 Compatibility Matrix 可用，预检已安装 PDRL/PDRV；
3. 停止 daemon；
4. 原子替换 PDCM binary/lib/catalog；
5. 保留用户 config，新增默认采用平台标准机制；
6. 启动并执行 version/discovery/health smoke；
7. unsupported PDRL ABI 允许 daemon Degraded，但 upgrade 必须明确告警并按发布 policy 判失败。

P0 不承诺无停机升级。

### 16.3 Uninstall

stop/disable service → 等待有界退出 → 删除 socket/runtime files → 删除 package files/unit。不得残留 PDCM 进程、socket 或 active unit。用户 config/log 的保留遵循平台 policy。

## 17. Release Documentation

每个 P0 release 必须包含：

1. Installation/Upgrade/Uninstall Guide；
2. CLI Reference；
3. Single-card Support Matrix；
4. P0 Metric Catalog；
5. Firmware Heartbeat Health Contract；
6. Public API Reference；
7. Error Code Reference；
8. Troubleshooting；
9. Compatibility Matrix；
10. Known Issues；
11. Release Notes。

不包含 Basic Diagnostic Catalog。Metric Catalog 未冻结时 release gate 为 BLOCKED_EXTERNAL。

## 18. Tests

### 18.1 CLI Golden

- 每个命令 human/json success/partial/error；
- help 与 parser schema 一致；
- 0/1/>1 Device；
- dmon N samples 和单项失败；
- heartbeat HEALTHY/WARNING/ERROR/UNKNOWN；
- Ctrl-C 清理；
- operation/diag 命令不存在。

### 18.2 Packaging

- clean install/start/version/help/discovery/stop/uninstall；
- upgrade compatible/incompatible；
- config preserve；
- 无 process/socket/unit 残留；
- no DT_NEEDED libpdrl；
- missing/bad/incompatible libpdrl 时 daemon Degraded；
- package 不含 runner、Diagnostic Catalog 或 libpdrl。

### 18.3 Acceptance IDs

| ID | Requirement |
|---|---|
| REL-ACC-01 | AC-1 生命周期和退出码通过 |
| REL-ACC-02 | help/local version 无 daemon 可运行 |
| REL-ACC-03 | discovery/dmon/health 与单卡 Catalog/API 一致 |
| REL-ACC-04 | incompatible CLI/daemon major Fail Fast |
| REL-ACC-05 | PDCM 产物无 libpdrl DT_NEEDED |
| REL-ACC-06 | PDRL 缺失或不兼容时 daemon 可诊断降级 |
| REL-ACC-07 | P0 包不含 diag/runner/额外 Health 声明 |
| REL-ACC-08 | AC-11 文档存在且示例实测 |

## 19. Completion Criteria

- command/JSON schema、exit code 和路径已冻结；
- no-DT_NEEDED 和 degraded startup 自动验证；
- package 在 FPGA/EMU clean fixture 完成生命周期测试；
- help、Catalog、API 和 docs 由 CI 交叉检查；
- P0 不包含 diag、runner、libpdrl 或多卡命令；
- TDD-08 未解阻前，Provider 路径/ABI golden 和 target release 保持 BLOCKED_EXTERNAL。
