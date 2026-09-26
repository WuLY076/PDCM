# TDD-02：Public API、Local IPC 与 Session

> **TDD Version**：v0.8.2-tdd.1  
> **Baseline RFC**：PDCM Architecture Draft v0.8.2  
> **Status**：Proposed for Implementation Review

## 1. Scope

本文冻结 libpdcm.so 公共 C ABI、BackendSelector、StandaloneBackend、EmbeddedBackend、PDCM Local Protocol、PdcmApiService、RequestRouter 和 SessionManager。业务语义由 TDD-03～06 提供；Provider 原生细节不进入本模块。

## 2. Public ABI Rules

1. 公共头文件必须可被 C11 和 C++17 编译器包含。
2. 公开函数使用 PDCM_API 与 C linkage。
3. 所有对象使用 opaque handle。
4. 可扩展结构首字段为 uint32_t struct_size 和 uint32_t version。
5. 输入内存在同步函数返回后不再使用，除非 API 明确复制并返回 ownership token。
6. 输出数组使用 caller buffer/two-call pattern；不得要求调用方跨库 free 内存。
7. 未知输入 enum 返回 INVALID_ARGUMENT；未知输出 enum 映射 UNKNOWN 并保留 raw value。
8. C++ exception 在边界转换为 PDCM_STATUS_INTERNAL。
9. public ABI major 不兼容时 pdcm_open Fail Fast；minor 通过 struct_size 兼容追加字段。
10. 函数返回调用级状态；批量项状态保存在结果项。

## 3. Handle Model

| Handle | Owner | 子资源 | Thread-safe | Close 行为 |
|---|---|---|---|---|
| pdcm_handle_t | caller | watches、subscriptions、selection snapshots | Yes | 阻止新调用，等待已进入调用，释放 Session |
| pdcm_watch_t | parent Session | one logical Watch | Yes | 删除该 owner requirement |
| pdcm_subscription_t | parent Session | bounded delivery queue | Yes | 停止新 callback，等待已开始 callback |

内部 handle header 至少包含 magic、type、ABI version、atomic state、backend ref 和 Session ID。所有 API 先校验指针、type、state 和 ABI。close/destroy 成功后将调用方二级指针置 NULL。

P0 不创建 Operation handle 或 Operation ownership reference。

## 4. Backend Selection

| Mode | 行为 | P0 |
|---|---|---|
| PDCM_MODE_STANDALONE | 连接本地 daemon | 生产默认、Required |
| PDCM_MODE_EMBEDDED | 创建独立 Core | Mock tests 和批准工具 |
| PDCM_MODE_AUTO | 先 standalone；只有显式允许才 fallback embedded | Supported |

默认 options：standalone、默认 UDS、默认 deadline、禁止 embedded fallback。Backend 在 open 时确定，handle 生命周期内不得切换。

libpdrl.so 加载或 ABI 协商失败不等于 Session 建立失败。standalone daemon 可返回 Degraded Session；embedded 也可在 Core 成功但 Provider UNAVAILABLE 时返回可用 handle。

## 5. Public API Surface

P0 导出：

~~~c
pdcm_status_t pdcm_open(const pdcm_open_options_t *, pdcm_handle_t **);
pdcm_status_t pdcm_version_get(pdcm_handle_t *, pdcm_version_info_t *);
pdcm_status_t pdcm_close(pdcm_handle_t **);

pdcm_status_t pdcm_entity_list(pdcm_handle_t *,
                               const pdcm_entity_filter_t *,
                               pdcm_entity_info_t *, size_t *);
pdcm_status_t pdcm_capability_query(pdcm_handle_t *,
                                    const pdcm_entity_ref_t *,
                                    pdcm_capability_set_t *);

pdcm_status_t pdcm_watch_create(pdcm_handle_t *,
                                const pdcm_watch_request_t *,
                                pdcm_watch_t **);
pdcm_status_t pdcm_watch_destroy(pdcm_watch_t **);

pdcm_status_t pdcm_query_latest(pdcm_handle_t *,
                                const pdcm_query_request_t *,
                                pdcm_query_result_t *);
pdcm_status_t pdcm_health_query(pdcm_handle_t *,
                                const pdcm_health_request_t *,
                                pdcm_health_result_t *);

pdcm_status_t pdcm_subscribe(pdcm_handle_t *,
                             const pdcm_subscription_request_t *,
                             pdcm_event_callback_t, void *,
                             pdcm_subscription_t **);
pdcm_status_t pdcm_subscription_close(pdcm_subscription_t **);

pdcm_status_t pdcm_operation_start(pdcm_handle_t *,
                                    const pdcm_operation_request_t *,
                                    pdcm_operation_id_t *);
pdcm_status_t pdcm_operation_get(pdcm_handle_t *,
                                  pdcm_operation_id_t,
                                  pdcm_operation_status_t *);
pdcm_status_t pdcm_operation_cancel(pdcm_handle_t *,
                                     pdcm_operation_id_t);
~~~

operation 三个符号只用于 ABI 预留，P0 固定返回 PDCM_STATUS_UNSUPPORTED。不得创建 ID、Provider call、状态、事件或审计记录。

config/set/group/fieldgroup/policy/profile/stats/topology/exporter 类公共函数不进入 P0 symbol allowlist。

## 6. Common Headers

所有 request 逻辑包含：

~~~c
typedef struct {
    uint32_t struct_size;
    uint32_t version;
    uint64_t request_id;
    uint64_t deadline_ns;
    uint32_t flags;
    uint32_t reserved;
} pdcm_request_header_t;
~~~

所有批量 result 逻辑包含：

~~~c
typedef struct {
    uint32_t struct_size;
    uint32_t version;
    uint64_t request_id;
    uint64_t catalog_generation;
    uint32_t item_count;
    uint32_t completeness;
} pdcm_result_header_t;
~~~

deadline_ns 是从调用开始计算的 monotonic duration。IPC 传输剩余 duration 和原 request_id，不传 wall-clock deadline。

## 7. Status Mapping

| 条件 | Public status |
|---|---|
| 所有请求项成功 | SUCCESS |
| 至少一项成功且至少一项失败 | PARTIAL_RESULT |
| 参数、结构版本、句柄非法 | INVALID_ARGUMENT |
| 能力不支持或多卡配置 | UNSUPPORTED |
| 组件尚未初始化 | NOT_INITIALIZED |
| daemon、Provider、PDRL 或 Device 暂时不可用 | UNAVAILABLE |
| peer 权限不足 | PERMISSION_DENIED |
| 同步 deadline 超时 | TIMEOUT |
| 配额/队列/容量不足 | RESOURCE_EXHAUSTED |
| caller buffer 不足 | BUFFER_TOO_SMALL |
| Entity/Watch/Subscription 不存在 | NOT_FOUND |
| Entity generation 已变化 | STALE_GENERATION |
| PDCM 内部不变量/序列化错误 | INTERNAL |

Operation API 在 P0 的返回优先级：无效 handle/必要指针仍返回 INVALID_ARGUMENT；结构可解析后固定返回 UNSUPPORTED，不执行授权、配额预留或下游调用。

## 8. PDCM Local Protocol

### 8.1 Transport

- AF_UNIX/SOCK_STREAM；
- 默认 /run/pdcm/pdcm.sock；
- server 使用 SO_PEERCRED 建立 principal；
- socket 默认 0660，owner/group 由 package 配置；
- 不监听 TCP。

### 8.2 Frame

~~~text
uint32 frame_length_be
uint16 protocol_major_be
uint16 protocol_minor_be
uint16 message_type_be
uint16 flags_be
uint64 request_id_be
bytes  protobuf_payload
~~~

规则：

- frame length 小于 header 或大于上限时关闭连接；
- protocol major 不匹配时返回一次 incompatible response 后关闭；
- minor 协商为双方可支持的较低版本；
- 同一连接允许并发 request，response 由 request_id 关联；
- protobuf field number 发布后不得复用，删除字段必须 reserved；
- malformed/unknown message 不得创建任何业务资源。

### 8.3 Handshake

首消息必须为 HELLO_REQUEST，包含 client library/protocol version、pid、nonce 和请求特性。响应包含 daemon version、协商协议、session_id、Core state、Provider load/compatibility summary、catalog generation 和 limits。

握手前不得创建 Session 业务 ownership。major 不兼容时 pdcm_open 返回 UNSUPPORTED。

### 8.4 Message Families

| Family | P0 |
|---|---|
| lifecycle/version | Active |
| discovery/capability | Active |
| watch | Active |
| query/health | Active |
| subscription | Active |
| operation | Schema reserved；handler 固定 UNSUPPORTED |

## 9. SessionManager

~~~mermaid
stateDiagram-v2
  [*] --> Handshaking
  Handshaking --> Active: HELLO accepted
  Handshaking --> Closing: reject or timeout
  Active --> Draining: daemon stopping
  Active --> Closing: close, EOF, protocol error
  Draining --> Closing: resources released or grace expired
  Closing --> Closed: cleanup complete
  Closed --> [*]
~~~

Session 拥有 Watch、Subscription 和 selection snapshot。清理顺序：

1. 从 subscription matching snapshot 移除；
2. 关闭 delivery queue；
3. 删除该 Session 的 Watch requirements；
4. 释放 selection 和 pending response；
5. 移除 Session。

共享 EffectiveWatch 只在最后一个 owner 离开后停止。

## 10. Quotas

握手响应返回该 principal 的有效上限。资源创建必须先原子 reserve，下游失败时 rollback。至少限制：

- outstanding requests；
- watches；
- 展开的 entity-metric pairs；
- subscriptions、events 和 bytes；
- response items/bytes；
- Fresh Read in-flight 数。

P0 无 Operation quota。

## 11. RequestRouter

固定处理顺序：

1. frame/protobuf；
2. Session/protocol feature；
3. struct/schema version；
4. public input validation；
5. Operation reserved fast-path；
6. peer authorization；
7. quota/deadline；
8. EntityRef/catalog generation；
9. 调用唯一 domain service；
10. 转换结果、记录完成并异步 enqueue response。

Router 不缓存业务数据、不计算 Health、不重试 Provider、不写 Catalog。

## 12. Authorization

| Permission | APIs |
|---|---|
| READ_BASIC | version、entity、capability、query、health |
| WATCH | watch、subscription |

授权基于 peer uid/gid 与静态 policy。embedded 由创建时注入 principal，不能默认 root。拒绝必须发生在创建下游资源之前。

Operation 在 P0 不形成 DIAG_BASIC 权限类别；它始终是不支持能力。

## 13. Callback Contract

- callback 只在 client-side executor 执行，不在 IPC reader、DataManager 或 Core lock 线程执行；
- 同一 subscription 保持 sequence 顺序，不同 subscription 可并发；
- callback 可以调用只读 API；
- 当前 callback 内关闭自身 subscription 时采用延迟关闭，不允许死锁；
- queue 有界，慢消费者按 TDD-05 接收 loss marker；
- subscription_close 返回后不得开始新 callback。

## 14. Two-Call Buffer

1. buffer 为 NULL 时返回 required count；
2. buffer 容量不足时只写完整前缀，返回 required total 和 BUFFER_TOO_SMALL；
3. 两次调用间 Catalog 变化时返回 STALE_GENERATION 或新 required count；
4. 只写 caller struct_size 声明的范围；
5. 多卡 Discovery 返回 UNSUPPORTED、detected count，不填充 entities。

## 15. API-specific Contract

| API | 关键行为 |
|---|---|
| pdcm_open | 非法 mode/path/deadline 拒绝；Provider unavailable 时可返回 degraded handle |
| pdcm_version_get | NULL handle 返回本地版本；有效 handle 额外返回 daemon、Provider 和 PDRL load/ABI 状态 |
| pdcm_entity_list | 正常只返回 0 或 1 Device；多卡时 UNSUPPORTED 且不返回第一张 |
| pdcm_capability_query | 只返回当前真实能力；P0 无 Operation Capability |
| pdcm_watch_create | selection 非空；period/freshness/capability 受 Catalog 约束 |
| pdcm_query_latest | 支持 cache/fresh policy 和逐项状态 |
| pdcm_health_query | 只接受 Firmware heartbeat；其他 subsystem item-level UNSUPPORTED |
| pdcm_subscribe | callback 非 NULL，队列参数有界 |
| pdcm_operation_* | 合法基础参数后固定 UNSUPPORTED，无副作用 |

## 16. Version API

pdcm_version_info_t 至少表达：

- local libpdcm/public ABI/local protocol；
- daemon/Core/protocol；
- Provider Contract version；
- Provider state：UNINITIALIZED、READY、UNAVAILABLE、SHUTDOWN；
- PDRL load state：NOT_ATTEMPTED、LOADED、FAILED；
- compatibility state：UNKNOWN、COMPATIBLE、UNSUPPORTED_ABI；
- failure phase：LOAD、ENTRY、ABI、NATIVE_INIT、DISCOVERY；
- sanitized stable reason code；
- PDRL/PDRV version，若可获得；
- target 和 catalog generation。

某个底层版本不可得时，该 item 独立标记，不使 Version 整体失败。禁止向普通用户直接回传任意 loader/native 原始字符串。

## 17. Tests

### 17.1 ABI

- C11/C++17 compile；
- public symbol allowlist；
- struct layout/version golden；
- old/new minor 兼容；
- exception/OOM 转换；
- operation symbols 存在但只返回 UNSUPPORTED。

### 17.2 Protocol

- fragmentation/coalescing、oversize、truncated、unknown type；
- major reject/minor negotiate；
- duplicate request ID、malformed protobuf；
- 并发 response correlation 和 disconnect race。

### 17.3 Session

- disconnect 只删除本 Session ownership；
- quota reserve/rollback；
- close 与 callback/request 并发；
- daemon restart 后旧 handle UNAVAILABLE；
- degraded handle 的 Version 可用、Provider-backed 请求失败明确。

### 17.4 Acceptance IDs

| ID | Requirement |
|---|---|
| API-ACC-01 | 只导出批准的 pdcm_* symbols |
| API-ACC-02 | standalone/embedded 对同一 Mock fixture 结果等价 |
| API-ACC-03 | protocol/public ABI major 不兼容 Fail Fast |
| API-ACC-04 | Session 断连完整释放其 Watch/Subscription |
| API-ACC-05 | malformed IPC 不 crash、不越界、不创建资源 |
| API-ACC-06 | degraded open 和 PDRL loader 状态可诊断 |
| API-ACC-07 | operation APIs 无条件不创建资源并返回 UNSUPPORTED |

## 18. Completion Criteria

- headers、protocol schema、symbol/version map 已评审；
- 每个 API 有 ownership、thread、deadline 和 error tests；
- operation schema 仅作为保留，不进入 domain service；
- 无 PDRL 类型、原生字符串或业务 Cache；
- TDD-03～06 的 mock service 可通过同一 Router 调用。

