# 本机工具总线（v0.3.2）

本页是 #5121（v0.3.0 底座）、#5376（v0.3.1 跨进程自进化闭环）与 #5391（v0.3.2 统计准入）的实现说明。只做同机协同：一条总线对应一个工作区、一份活 AST；不引入总线守护进程，不做跨机、RDMA 或 CXL。v0.3.1 在底座上交付多对端并发服务、KEEP 原子应用回活 AST、候选证据五元绑定、约束最小安全子集与金丝雀观察期；v0.3.2 把统计准入总线化——协调者自己重算判决。

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
- 频道写入按频道各记一个 loan 账本（默认 8 个）；一个频道用尽只拒绝该频道，别的频道仍可 `Reserve`。能力里的 `loans` 配额按频道收紧；`Publish` 归还，断开时每个频道未归还的 loan 全部收回。
- 套接字一断（EOF、进程退出、keepalive 失败），会话结束：协调者收回未提交 loan，把未 KEEP 的试改按 `ROLLBACK` 处理，并停止接受该会话的提交。即使映射还在，也不得只凭共享内存继续信任对端；对端必须先停止解引用再 `munmap`。

## 当前覆盖

已实现（v0.3.0 底座 + v0.3.1 闭环）：控制帧、握手与拒绝原因、memfd 封印与对端检查、eventfd 门铃、loan 计数与 `EAGAIN`/`ENOSPC`、断开回收、能力与 grant 树、worldline 的 trial/keep/rollback、约束门、订阅频道与 `Subscribe`/`CatchUp` 路由、`std/bus` 公共面；多对端并发服务与会话隔离（#5377）、KEEP 原子应用回活 AST（#5378）、候选证据五元绑定与防陈旧提交（#5379）、约束最小安全子集与保护集镜像（#5380）、金丝雀两阶段提交与退化自动撤回（#5381）。跨主机不在本版。

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

协调者在节尾的预留容量里追加记录，提交时更新该节 length/count/digest/generation，并把映像头 committed_gen 推进到下一个偶数代次。订阅者只在稳定代次上读；游标记录上次的 generation/count，下一次只取新增记录。代次是映像全局的（审计/约束写入也推进），游标代次不超前于节代次且 count 不超量即按 count 切片；任一超前（可能已重建）返回完整快照，绝不静默错位。gen=0 且 count=0 读全量。容量用尽是硬错误，不扩容。

控制面接线：

- `Subscribe` 的 channel 1..4 分别对应指标、CCT、补丁、worldline；回 `ChannelBody`（generation、count、offset、length、snapshot=1）。
- `CatchUp` 带上次 generation/count；协调者只返回新增记录范围，游标代次或 count 超前时返回完整快照（snapshot=1）。回复里 count 是本次范围的记录数，total 是频道已提交总数（下一次游标）。
- `Reserve`/`Publish` 对 channel 1..4 直接操作该节尾部的预留容量；Publish 成功后回新的 generation/count，并把该次发布写进审计。
- 对端只有在拿到 `publish` 或 `trial` 时才以可写方式映射映像；只读会话仍是 `PROT_READ`。

### 活跃生产者（#5122）

三条投影直接从活工作区取记录，不再由调用方构造：

- `(bus:project-cct session)` → `(ok count live)`：默认遍历当前工作区 FlatAST 的 `Call` 节点做静态调用点投影（`samples=1`，`flags` 最低位标记静态）；设置 `AURA_BUS_CCT=1` 后改为发布线程内的真实调用上下文树：tree-walker 的 `apply_closure` 与 IR 的 `call_closure` 都记录，每个上下文按 (父上下文, 被调身份) 建节点，累加 samples 和独占 self-ns，`parent` 是批内父下标，`live=1`。快照每会话只发布一次（静态与 live 路径同契约，#5386），重复调用返回同一 count。Recorder 是 thread-local，多 fiber 生产者按 fiber 投影。
- `(bus:project-patches session)` → `(ok generation total next-mutation skipped)`：把游标之后每条带回滚数据的已提交变更写成补丁记录；`node`/`field_offset`/`new_value` 来自变更日志，`worldline` 是当时生效的 rebind 槽位。第 63 位置位或节点越界的记录跳过并计入 `skipped`。
- `(bus:project-worldlines session)` → `(ok generation total next-mutation)`：按变更顺序为每个 rebind 写一条 worldline，`id` 是真实 mutation id，`parent` 是父 mutation 的槽位，状态为 kept/rolled，补丁区间取该槽位已发布的补丁范围。

槽位按 mutation log 顺序分配，因此 `PatchRecord.worldline` 与 worldline 表下标一致，读者可以由槽位反查 `WorldlineRecord.id`。先投影补丁再投影 worldline，worldline 记录里才有补丁区间；反过来则区间为 0。`bus:project-nodes` 现在优先投影 `workspace_flat`（`set-code`/`eval-current` 建立的持久工作区），没有时才退回当前求值的 AST。

黄金夹具在 `tests/fixtures/bus/golden.img`（2272 字节，`test_bus_golden` 逐字节比对）：五条频道节加审计节，两个进程通过同一 memfd 映射读出相同的 `committed_digest`；故意改已提交字节会校验失败，审计尾仍可追加。改布局后跑 `AURA_BUS_WRITE_GOLDEN=1 ./test_bus_golden` 重新生成并审阅二进制差异。

查询面（需要 `read`）：`(bus:hotspots session top)` 在客户端聚合 CCT，按 `mid` 汇总 samples/self-ns 后降序取前 N；`(bus:diff-worldlines session id-a id-b)` 按真实 worldline id 取两条记录，比较状态、约束结果、指标偏移和补丁区间，返回两侧不一致的节点行 `(node in-a in-b field-a field-b value-a value-b)`。两者都直接读已映射的映像，不发控制帧。

## 约束与审计

- 协调者持有 `ConstraintStore`：一组可执行规则加版本号。安装新集或回滚都要求 `constraint-write`，并分别记 `ConstraintChange` / `ConstraintRollback` 审计。
- 工作区允许策略现在是同一份版本化集合的一部分（规则-only 安装保留当前策略；带策略的安装原子替换两者，回滚一起恢复）。`(bus:install-constraint session name source)` 在 `constraint-write` 下把规则源下发给协调者；协调者只接受已注册的 `name`（注册表把名字绑定到编译后谓词），未注册直接拒绝，不会静默放行。`(bus:rollback-constraints session)` 回到上一版本。两者都写 `ConstraintChange` / `ConstraintRollback` 审计并刷新映像约束节。
- 每次 `KEEP` 先跑当前约束集。失败回 `EPERM`，消息带规则名和原因；worldline 保持 pending，活工作区不变，断开时按 rollback 处理。控制面没有「关闭约束再 keep」的消息，本地 `AURA_SANDBOX=off` 也不影响已附着会话。
- 审计事件带约束版本、结果、原因、时间戳和发起 token；订阅、发布、trial、约束结果、KEEP/ROLLBACK、grant/revoke、断开都记录。可按 worldline / actor / since-seq 过滤（`bus:audit`、`bus:audit-filter`），自动轮次和人工试跑写同一份日志。
- 协调者同时把每个事件追加进映像的审计节（若构建方预留了该节）：只追加、序号递增、只写下一个偶数代次，并重算节摘要；节满或没预留时控制面日志仍然完整，映像写入跳过。`AuditRecord` 只装 kind/actor/target/value，约束原因和版本仍从控制面查询读取。
- 当前约束集同时写进映像的约束节：`ConstraintSetHeader` 带版本号，对象区是每条规则的 `(name, EDSL source)` 序列。附着时和每次 `install_constraints` 后刷新，`(bus:constraints session)` 读回 `(ok version count rows)`，每行 `(name source)`；执行仍用协调者持有的编译后谓词，源文本供其它工具核对与复现。

## 协调者宿主(#5128)

#5121 的进程模型:协调者就是拥有活工作区的那个 Aura 进程,不另起守护进程。
`std/bus` 的宿主面把它接上:

- `(bus:host workspace [image-bytes] [path])` → `(ok path workspace)`。在本进程
  启动协调者:监听默认套接字(或指定路径),按交付的映像 schema 建初始映像
  (五条记录频道 + 约束节),后台线程服务对端。按工作区幂等;重复调用返回同一
  路径。映像默认 256 MiB，可指定更小。频道容量随映像尺寸分配：每频道保底
  4096 条，超出保底的空间在五频道间均分（#5387）；用尽仍是硬错误。
- `(bus:unhost workspace)` 停止服务、收回线程、unlink 套接字;
  `(bus:host? workspace)` → `(ok 0|1)`。
- 角色约定:`1` perf、`2` harness、`3` maintainer、`0` 通用。宿主策略允许这四个
  角色请求能力子集;未知角色拒绝。沙箱预设取自 `AURA_SANDBOX`(ci 去
  constraint-write,prod 再去 grant),附着时裁剪。
- 内置可安装约束名(注册表,默认不安装——约束由专家下发,不内置生效):
  `allow-small`(new-value ≤ 100,#5127 测试契约)与 `value-fits-immediate`
  (第 63 位为 0 的映像立即数不变量)。持 constraint-write 的对端用
  `bus:install-constraint` 安装注册名;未注册名拒绝。
- **多对端并发服务**(#5377):协调者用一条 poll 事件循环同时服务所有已附着
  对端(监听套接字 + 唤醒 eventfd + 每会话控制套接字)。每会话独立持有
  conn_fd、session id、root grant、能力、loan 账本、暂存配额与订阅游标
  (`PeerSession`);共享状态(映像、grant 树、worldline 台账、约束、审计)
  只由协调者线程串行推进,代次推进无需加锁。某对端崩溃/EOF/心跳超时只
  收回该会话未归还 loan、把其未决 trial 标 Rollback 并写审计,绝不影响
  其他在线会话。宿主可经 `bus:grant`(带 worldline 作用域)实时委派单条
  worldline 的 keep+rollback 给裁判,`bus:revoke` 递归失效。提交新代次后
  向其他已订阅会话的门铃 eventfd 发通知,裁判/观察者无需轮询。
- **被拒绝的附着只结束该连接**(#5383):未知角色、workspace 不符、schema/uid
  不符、握手半途 EOF 等拒绝会关闭该连接并继续 accept,不停服务循环;只有
  unhost/监听失效才退出。一个配置错误的对端不会瘫痪整条总线。
- 宿主进程自己也可以 `bus:attach` 成为对端(同进程经套接字,统一路径)。
- **映像跨会话持久**(#5128 完成线):一条总线一份映像。首个对端附着时创建并
  封印映像,之后每个对端拿到同一个 memfd 的新映射——前一个工具提交的 CCT /
  指标 / 补丁 / worldline 记录对下一个工具可见,断开只回收未 KEEP 试改与
  loan,不回收已提交证据。宿主自留一份映射与 fd 引用,对端 detach 不会带走
  映像;unhost 才释放。映像已存在时请求更小的映像尺寸是硬错误,不静默缩容。
- **校验不变量在发布时执行**:补丁节点必须落在节点节内、指标键是合法立即数
  (第 63 位为 0),发布即拒绝,绝不提交会让下一个对端附着校验失败的记录。
  `bus:project-nodes` 的 parent/first_child/next_sibling 用批内下标,sym 在
  无符号表时为 kImageNoIndex。
- **trial 台账投影到 worldlines 频道**:协调者把每个 trial(状态 pending)及
  其 KEEP/ROLLBACK/断开回滚的最终状态按追加方式写进 worldlines 节,订阅者
  (harness)因此看得到每条试跑与裁决,而不只是生产者自己工作区的投影。

测试:`test_bus_host`(#5128)——真实 aura 进程,只用 std/bus:宿主脚本
`bus:host` 后服务,对端脚本附着/发布/查询/trial/约束下 KEEP 与拒绝/ROLLBACK/
读同一份审计;另覆盖无宿主附着的 transport-failed、同进程宿主+附着、幂等与
unhost 后套接字清理,以及三对端顺序服务(perf 发布并装约束 → harness 读同一
映像/审计 → maintainer 的 KEEP 被 perf 装的约束门控),含越界补丁节点在发布
时被拒绝。#5383 回归:badrole/wrongws/裸连接 EOF 三种拒绝之后,合法对端仍能
附着、发布、detach(修复前服务循环在首次拒绝即退出,后续 attach 全部挂死)。

v0.3.1 闭环测试:`test_bus_multi_peer`(#5377,三对端并发 + 提议进程 SIGKILL
隔离 + 越权 KEEP 拒绝)、`test_bus_keep_apply`(#5378,源载体补丁原子应用回活
AST,非法语法拒绝且源码逐字节不变)、`test_bus_evidence_binding`(#5379,五元
证据匹配/伪造/陈旧/缺失 + v1 映像兼容)、`test_bus_constraints_subset`(#5380,
保护集镜像 + pure-define-only 纯度门 + constraint-write 权限隔离)、
`test_bus_canary_rollback`(#5381,低于底线自动撤回 + 窗口期满封存 + 并发
canary 拒绝)、`test_bus_closed_loop_e2e`(#5382,三进程正常闭环 + 五类故障
注入矩阵 + 审计可重放)。全部接入 ctest(`test_bus_*`)。

## std/bus 公共面

`std/bus` 只包一个内核桥 `bus-call`（建立共享映像、跨进程附着、能力检查和原子发布代次是 Aura 写不出来的）。Aura 侧动词：

- `(bus:attach path workspace role caps channels)`，path 空串用默认工作区路径；返回 `(ok session size caps)`。
- `(bus:detach session)`
- `(bus:subscribe session channel)` → `(ok generation count offset length snapshot)`
- `(bus:query session channel since-generation since-count)` → `(ok generation count snapshot rows)`，rows 是解码后的记录列表。
- `(bus:publish session key value)` → `(ok generation count)`；另有 `bus:publish-cct` / `bus:publish-patch` / `bus:publish-worldline` 把其余三种记录投影进对应频道（parent/worldline 用 -1 表示无）。
- `(bus:project-nodes session)` / `(bus:project-cct session)` / `(bus:project-patches session)` / `(bus:project-worldlines session)` 从活工作区直接投影四类记录（见「活跃生产者」）。
- `(bus:trial session channel)` → `(ok worldline)`；`(bus:keep session worldline node value)` 跑当前约束 + 证据门 + 活 AST 原子应用；`(bus:rollback session worldline)`。
- `(bus:grant session bits channels loans expires worldline)` → `(ok token)`，只能衰减；`(bus:revoke session token)` 递归失效；`(bus:grant-activate session token)` → `(ok bits worldline)`，把本会话能力收窄到 token 的有效(Scoped)能力——裁判激活宿主委托的单 worldline keep+rollback(#5377)。
- `(bus:audit session worldline)` → `(ok count truncated events)`；行是 `(kind token worldline result version timestamp-ms seq reason)`。`(bus:audit-filter session worldline actor since-seq)` 同一份日志按发起者和序号过滤，0 表示不过滤。

### v0.3.1 闭环动词(#5378/#5379/#5381)

- `(bus:publish-source session source-text)` → `(ok index generation)`：把候选 define 源码追加进映像字符串表,返回条目下标。提议方随后 `(bus:publish-patch session node bus:source-field index worldline generation)` 发布源载体补丁(field_offset = `bus:source-field` = 4294967280,new_value = 字符串表下标)。KEEP 时协调者把源码经 `mutate:rebind` + `eval-current` 原子应用回宿主活工作区;任一步失败即回滚快照,活 AST 逐字节不变。
- `(bus:candidate-digest session worldline)` → `(ok digest)`：协调者对该 trial 已提交补丁字节流的 FNV-1a 摘要(截断低 62 位,fixnum 安全)。
- `(bus:bind-evidence session worldline baseline candidate workload seed verifier)` → `(ok worldline)`：把五元证据绑定到未决 trial。KEEP 强制核对 `baseline_gen == 当前工作区代次`、`candidate_hash == digest(trial.patches)`、workload/seed/verifier 非零;任一缺失或不一致拒绝 KEEP(`stale-baseline` / `candidate-hash-mismatch` / `incomplete-evidence` / `missing-evidence`)。证据尾随 worldline 记录追加进映像(layout_version 2,v1 读者安全跳过)。
- `(bus:evidence session worldline)` → `(ok baseline candidate workload seed verifier)`：读回某 worldline 最终记录的证据尾。
- `(bus:canary-confirm session worldline)` → `(ok worldline)`：裁判提前封存活跃金丝雀。
- 宿主环境变量:`AURA_BUS_EVIDENCE=1` 开启证据强制门;`AURA_BUS_CANARY_WINDOW=N` 开启 N 个指标 tick 的观察期;`AURA_BUS_CANARY_FLOOR=F` 设退化底线;`AURA_BUS_CANARY_KEY=K` 指定驱动观察期的指标键。观察期内连续两次低于底线自动撤回(恢复前一版快照,写 `canary-reverted` 审计);窗口期满达标自动封存为 Kept;同一时刻至多一个活跃 Canary,第二个 KEEP 被 `canary-active` 拒绝。默认全关,保持 v0.3.0 行为。
- 约束最小安全子集(#5380):宿主预注册 `pure-define-only`(候选体禁 io/net/socket/ffi/mutate/set!/bus 等副作用算子)、`target-allowlist`、`metric-floor-bound` 三个参数化谓词;`std/contract` 的保护集经 #5317 digest 镜像进 KEEP 前的结构门,触碰受保护 define 一律拒绝。持 constraint-write 的会话才能安装/回滚约束。
- 失败是 `(error code message)`，不是 `#f`；`bus:ok?` / `bus:code` / `bus:message` 读取。错误码含 `schema-mismatch`、`seals-missing`、`loan-exhausted`、`image-full`、`validation-failed`、`permission`、`unregistered-id`、`transport-failed`。

`(api-reference)` 现在也列出这些动词、所需能力和失败码。审计有两个面：控制面日志（带原因，供 `bus:audit`）和映像审计节（只追加，供映射方直接遍历）。

### v0.3.2 统计准入(#5391)

裁判负责测量，协调者负责裁决。裁判把候选的 held-out 证据绑到自己的 trial 上，协调者**不信裁判的汇总结论**：它从绑定的原始窗口重算均值、样本标准差与标准误，按**自己观察到的**候选族规模做多重比较校正（Bailey/López-de-Prado 折价 `E[max of N 正态] * se`，与 `std/validation` 的 `val:deflate` 同一套数学），然后裁决。结论是定点数，同一份证据永远重算出逐字节相同的判决。

- `(bus:bind-gate session worldline round workload seed verifier folds threshold control verdict windows)` → `(ok worldline)`：绑定一份候选的 held-out 证据。`windows` 是逐折 out-of-sample 指标增量列表（百万分之一为单位，可负）；`round` 是候选所属的准入轮次（族），`workload`/`seed`/`verifier` 必须与已绑定的五元证据一致，`control=1` 标记本轮的负向对照（已知有害候选）。
- `(bus:gate-evidence session worldline)` → `(ok verdict family adjusted threshold fingerprint)`：读回协调者自己的判决（verdict 1 = 放行，2 = 拒绝；adjusted 是折价后的均值，单位 ppm）。
- 准入拒绝原因：`missing-gate-evidence`（没有统计证据）、`missing-negative-control`（本轮没有负向对照）、`negative-control-refused`（对照永不应用）、`negative-control-accepted`（统计放行了已知有害候选 = 门失效）、`gate-rejected`（统计不显著）、`gate-evidence-mismatch`（证据与五元不是同一次评估）。
- 判决随 worldline 记录追加进映像（新增尾字段 `gate_verdict`/`gate_family`/`gate_adjusted_ppm`/`gate_threshold_ppm`/`gate_fingerprint`，layout_version 3；v1/v2 映像照读，尾字段默认零）。
- 负向 KEEP 预算：协调者盯住最近若干次 KEEP。被撤回的 KEEP（可观测期退化自动撤回或裁判回滚）与被统计放行的对照都算负向事件；超过预算就把折价乘子翻倍并写 `GateTighten` 审计（准入自动收紧）。
- 宿主环境变量：`AURA_BUS_GATE=1` 开启准入门；`AURA_BUS_GATE_THRESHOLD`（放行门槛，ppm）、`AURA_BUS_GATE_MIN_WINDOWS`（最少 held-out 窗口）、`AURA_BUS_GATE_BUDGET`（负向 KEEP 预算，ppm）、`AURA_BUS_GATE_WINDOW`（预算滑动窗口大小）、`AURA_BUS_GATE_CONTROL=0`（不强制负向对照）。默认全关，保持 v0.3.1 行为。

### v0.3.2 审计回放(#5392)

闭环要「跑坏了能证明」：审计必须可回放，裁决必须可确定性复现。回放是协调者侧的一次**只读**扫描——它从控制面日志与映像里重构 worldline 谱系，校验完整性，并从绑定的证据重算判决。

- `(bus:audit-replay session)` → `(ok events worldlines keeps findings first-seq first-kind first-reason nodes)`。`findings = 0` 表示谱系完好；`first-kind` 指第一处缺陷的类别：1 孤儿事件（事件指向从未开启的 worldline）、2 断裂 parent 链、3 序号回退、4 映像审计节与控制面日志不一致、5 KEEP 缺约束结论、6 KEEP 缺准入判决、7 判决与重算不符、8 证据与五元不符。每行 node 是 `(worldline parent state trial-seq keep-seq verdict-seq constraint reproduced fingerprint timestamp-ms baseline candidate workload seed verifier gate-verdict gate-family adjusted reason)`。
- `(bus:audit-evo session)` → `(ok text)`：同一条谱系渲染成 **evo schema v2** 的 JSONL（每行一个 JSON 对象，见 docs/evo.md），`aura evo log` 可直接读总线闭环日志。
- 确定性复现：回放用判决记录里的策略快照（门槛、折价乘子、最少窗口数）从绑定的窗口重算判决，逐字节比较；判决是定点数，同一份证据两次重算必然一致。候选哈希会拿 trial 的补丁字节重算比对——篡改 `candidate_hash` 或篡改判决都会被检出。
