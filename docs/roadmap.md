# Aura 路线图

**更新：2026-05-25 — 实测后清除非真实 bug，仅保留确认为 TODO 的项**

---

## ✅ 已完成

| Phase | 内容 | 状态 |
|:------|:-----|:----:|
| Core | 核心求值器（tree-walker + IR 双路径 + TCO） | ✅ |
| Core | IR 管线（lower → passes → IR interpreter → JIT → AOT） | ✅ |
| Core | LLVM ORC JIT（38 opcode → native, 7.55× vs TW） | ✅ |
| Core | 增量编译（ArenaGroup + 缓存 + 热替换 + IR import） | ✅ |
| Core | AOT 56 emit 全部通过 | ✅ |
| EDSL | `query:def-use/reaches/effects/children/node/node-type/root` | ✅ |
| EDSL | `query:pattern` / `query:find` | ✅ |
| EDSL | `mutate:splice/wrap/rebind/replace-value/remove-node/insert-child/tweak-literal` | ✅ |
| EDSL | `ast:snapshot/restore/diff/list-snapshots` | ✅ |
| EDSL | `eval` / `eval-expr` / `eval-current` / `current-source` | ✅ |
| EDSL | `compile:status` / `typecheck-current` | ✅ |
| Workspace | `workspace:create/switch/list/delete/lock/merge/discard/sync-from` + COW | ✅ |
| Messaging | `send/recv/my-id/reply/session-active?/mailbox-count` | ✅ |
| Synthesize | `synthesize:register-template/fill/define/pipeline/optimize` | ✅ |
| Rule | `rule:define/apply/apply-all/list/list-violations/status/enable/disable/remove/save/load` | ✅ |
| Type system | Sound Gradual Typing（coercion + occurrence + let-poly） | ✅ |
| Type system | `:Int` / `:Bool` / `:String` / `:Float` 标注语法 | ✅ |
| Type system | `define-type` ADT + `match` 穷尽性检查 | ✅ |
| Type system | M4 Linear 所有权（move/borrow/mut-borrow/drop） | ✅ |
| Keywords | `:foo` 自求值关键字支持 | ✅ |
| Stdlib | `std/rule` / `std/pipeline` / `std/list` / `std/json` / `std/csv` 等 32 个模块 | ✅ |
| C FFI | `c-func` / `dlopen/dlsym` + 类型签名 | ✅ |
| Compile-time reflection | P2996 `auto_to_json` / `auto_serialize` | ✅ |
| Serve | `--serve` JSON-line 多 session 协议 | ✅ |
| Serve-Async | ucontext fiber + epoll 调度器 + eventfd mailbox | ✅ |
| Serve-Async | `--serve-async` 多 session fiber 调度 + stdin edge-triggered | ✅ |
| Serve-Async | `pop_message` fiber yield + `g_fiber_block` 回调 | ✅ |
| Serve-Async | send/recv 全链路通信（session create → send → recv yield → resume） | ✅ |

---

## 🔴 真实遗留问题

| 优先级 | 任务 | 工时 | 说明 |
|:------:|:-----|:----:|:-----|
| P2 | **`synthesize:optimize` fitness 基于代码长度** | 1d | 需 benchmark 驱动的真实 fitness |
| P3 | 权限模型（module / symbol whitelist） | 3d | mutation 只允许特定 scope |
| P3 | eval 资源限制（CPU / memory / recursion depth） | 3-4d | serve 模式安全沙箱 |

## 🔭 前瞻

| 任务 | 说明 |
|:-----|:------|
| VS Code / Cursor 插件 | 原生 EDSL 支持 |
| LSP / 包管理 / 自举 | 独立长期项目 |
| 分布式 EDSL | 多机共享 AST workspace |
| 形式化证明接口 | 关键 mutate 走 proof 而非 test |
| LangGraph / CrewAI 集成 | 适配主流 Agent 框架 |
