# 本机工具总线（v0.3.0）

本页是 #5121 的实现说明。v0.3.0 只做同机协同：一条总线对应一个工作区、一份活 AST；不引入总线守护进程，不做跨机、RDMA 或 CXL。

## 默认传输

- 发现：`$XDG_RUNTIME_DIR/aura/bus/<workspace-id>.sock`，权限 `0600`，只接受同一 uid。测试或无 `XDG_RUNTIME_DIR` 时回退到 `/tmp/aura-<uid>/bus/`。
- 控制面：Unix 流套接字上的定长帧。帧头是 `len u32 | type u16 | flags u16 | session u64`，体只带频道、映像内偏移、长度、代次和 worldline；CCT、补丁和指标本体只出现在共享映像里。
- 数据面：协调者 `memfd_create(MFD_ALLOW_SEALING | MFD_CLOEXEC)`，`ftruncate` 后加 `F_SEAL_SHRINK | F_SEAL_GROW`；映射之后不加 `F_SEAL_WRITE`。memfd 和 eventfd 只在握手时经 `SCM_RIGHTS` 各传一次，之后控制帧不再重传 fd。
- 对端先 `F_GET_SEALS`，缺 shrink/grow 就拒绝；再按 #5122 校验映像头、节目录和已提交节摘要，才进入已附着状态。

## 握手

`Hello` 带主次版本、schema id、layout version、pid、角色、请求的能力、workspace id 和请求的映像大小。协调者核对版本、schema、workspace、uid，创建并封印映像，回 `HelloOk`（session、授予的能力、映像大小、schema id、已提交字节数）；拒绝时回 `HelloReject` 带原因码，不传 fd。

## 反压与断开

- 反压是 loan / release 加每会话未读代次上限，不是带宽时延积窗口。`Reserve` 借一个暂存槽：成功回 `Credit`（含映像内偏移与长度），loan 用尽回 `EAGAIN`，暂存窗口用尽回 `ENOSPC`；两者都是硬错误，不覆盖未读数据、不在线扩容。
- 每个会话固定一个暂存窗口（当前 64 KiB，受映像已提交区之后的剩余空间约束）；提交后也不扩容。
- 套接字一断（EOF、进程退出、keepalive 失败），会话结束：协调者收回未提交 loan，把未 KEEP 的试改按 `ROLLBACK` 处理，并停止接受该会话的提交。即使映射还在，也不得只凭共享内存继续信任对端；对端必须先停止解引用再 `munmap`。

## 当前覆盖

已实现：控制帧、握手与拒绝原因、memfd 封印与对端检查、eventfd 门铃、loan 计数与 `EAGAIN`/`ENOSPC`、断开回收。

尚未实现（属于 #5124-#5127）：能力授予/撤销与 grant 派生、worldline 的 trial/keep/rollback 语义、订阅频道与 `Subscribe`/`CatchUp` 路由、`std/bus` 公共面。跨主机不在 v0.3.0。

## 能力

- 位：`read`、`publish`、`trial`、`keep`、`rollback`、`grant`、`constraint-write`。订阅、查询和审计读取不是新位，它们是 `read` 加频道掩码。
- 每个能力还带频道掩码、workspace、可选 loan 配额、到期代次和 worldline 作用域。派生只能缩小，不能加位、加频道、放宽配额/到期/作用域，也不能换 workspace。
- 附着时按工作区允许策略授予请求的子集；角色（perf/harness/maintainer）只是提示，未知角色默认拒绝。
- 只有持有 `grant` 的会话才能再授权，且形成派生树；撤销祖先会递归失效。
- `keep` / `rollback` 只作用于本会话开的 trial，或能力显式 scoped 到的 worldline；`keep` 还要求该 trial 的 loan 尚未归还。
- 会话结束：名下 pending trial 全部按 rollback 处理并写审计，未提交 loan 收回。
- 沙箱预设：`dev` 不裁剪；`ci` 去掉 `constraint-write`；`prod` 再去掉 `grant`。能力不能跨工作区携带。

## 增量频道

四条频道各有自己的节、代次和容量：

- `metrics`：键走符号表，值是合法立即数（第 63 位为 0）。
- `cct`：父下标只许向后，mid 用低 62 位，node 指向节点节。
- `patches`：字段偏移、新值、worldline、代次；旧值由补丁日志保留。
- `worldlines`：状态、约束结果、指标偏移、补丁区间、目标指标。

协调者在节尾的预留容量里追加记录，提交时更新该节 length/count/digest/generation，并把映像头 committed_gen 推进到下一个偶数代次。订阅者只在稳定代次上读；游标记录上次的 generation/count，下一次只取新增记录。游标落后到已重建（count 变小）或超前时返回完整快照，绝不静默错位。容量用尽是硬错误，不扩容。

控制面接线：

- `Subscribe` 的 channel 1..4 分别对应指标、CCT、补丁、worldline；回 `ChannelBody`（generation、count、offset、length、snapshot=1）。
- `CatchUp` 带上次 generation/count；协调者只返回新增记录范围，落后到重建时返回完整快照（snapshot=1）。
- `Reserve`/`Publish` 对 channel 1..4 直接操作该节尾部的预留容量；Publish 成功后回新的 generation/count，并把该次发布写进审计。
- 对端只有在拿到 `publish` 或 `trial` 时才以可写方式映射映像；只读会话仍是 `PROT_READ`。

## 约束与审计

- 协调者持有 `ConstraintStore`：一组可执行规则加版本号。安装新集或回滚都要求 `constraint-write`，并分别记 `ConstraintChange` / `ConstraintRollback` 审计。
- 每次 `KEEP` 先跑当前约束集。失败回 `EPERM`，消息带规则名和原因；worldline 保持 pending，活工作区不变，断开时按 rollback 处理。控制面没有「关闭约束再 keep」的消息，本地 `AURA_SANDBOX=off` 也不影响已附着会话。
- 审计事件带约束版本、结果和原因，可按 worldline 过滤；自动轮次和人工试跑写同一份日志。

## std/bus 公共面

`std/bus` 只包一个内核桥 `bus-call`（建立共享映像、跨进程附着、能力检查和原子发布代次是 Aura 写不出来的）。Aura 侧动词：

- `(bus:attach path workspace role caps channels)`，path 空串用默认工作区路径；返回 `(ok session size caps)`。
- `(bus:detach session)`
- `(bus:subscribe session channel)` → `(ok generation count offset length snapshot)`
- `(bus:query session channel since-generation since-count)` → `(ok generation count snapshot rows)`，rows 是解码后的记录列表。
- `(bus:publish session key value)` → `(ok generation count)`；另有 `bus:publish-cct` / `bus:publish-patch` / `bus:publish-worldline` 把其余三种记录投影进对应频道（parent/worldline 用 -1 表示无）。
- `(bus:trial session channel)` → `(ok worldline)`；`(bus:keep session worldline node value)` 跑当前约束；`(bus:rollback session worldline)`。
- `(bus:grant session bits channels loans expires worldline)` → `(ok token)`，只能衰减；`(bus:revoke session token)`。
- `(bus:audit session worldline)` → `(ok count truncated events)`，事件含 kind/worldline/result/constraint-version/reason。
- `(bus:grant session bits channels loans expires worldline)` → `(ok token)`，只能衰减；`(bus:revoke session token)`。
- `(bus:audit session worldline)` → `(ok count truncated events)`，事件含 kind/worldline/result/constraint-version/reason。
- 失败是 `(error code message)`，不是 `#f`；`bus:ok?` / `bus:code` / `bus:message` 读取。错误码含 `schema-mismatch`、`seals-missing`、`loan-exhausted`、`image-full`、`validation-failed`、`permission`、`unregistered-id`、`transport-failed`。

`(api-reference)` 现在也列出这些动词、所需能力和失败码。审计仍是进程内日志（还没写进映像审计节）。
