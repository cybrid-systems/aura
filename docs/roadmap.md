# Aura — 路线图（TODO）

**更新：2026-05-23**

**当前定位**：语言核心完备（Tree-walker + IR + JIT、Sound Gradual Typing、ADT、Linear Ownership），
已进入「让 AI 自治编辑更可靠」的迭代阶段。

**当前基准**：Grok 77/85（90.6%）、MiniMax 74/85（87.1%）、DeepSeek 72/85（84.7%）。
新增的 28 个类型系统任务（15 个）是主要拉分项，平均通过率 ~60%。

---

## ✅ 已修复（2026-05-23）

| # | 问题 | Commit |
|---|------|:------:|
| 1 | Blame：`(+ 1 "hello")` 输出 stderr 错误 | `052cb19` |
| 2 | `eval_flat` 缺失 Linear/Move/Borrow/Drop 节点 | `50208da` |
| 3 | `(: x Int)` 无绑定正确报错 | `82dfaf4` |
| 4 | `(: name Type val)` 三参数解析 | `afe96fd` |
| 5 | `#<procedure>` 系统 prompt 警示 + hint 强化 | `f8166c7` |

## P1 — 功能缺口

| # | 任务 | 当前状态 | 预估 |
|---|------|:--------:|:----:|
| 5 | **模块 import 类型签名传播** — `require`/`import` 的绑定大部分推断为 `Dynamic`。AI 编辑大项目时反馈不够精准。 | 大部分 Dynamic | 2d |
| 6 | **`let` 泛化** — Union-Find 约束求解器已实现，但 `let` 绑定不做泛化。`TypeScheme::is_poly` 字段存在但始终为 false。 | is_poly=false | 1-2d |
| 7 | **match 穷尽性检查** — `define-type`/`match` 类型推断已实现，但 match 不检查模式是否覆盖所有 variant。 | 缺检查 | 1d |
| 8 | **树遍历器 CastOp 覆盖率** — if 分支已插 CastOp，但 Pair 构建、Set!、宏展开产物的 CastOp 覆盖率仍需加强。 | 部分覆盖 | 1d |
| 9 | **运行时 blame 扩展** — BlameParty/BlameInfo 框架已实现，覆盖 JIT 路径，但解释器/IR 路径薄弱。 | JIT 有，TW 缺 | 1d |
| 10 | **M4 运行时违规检测** — 编译期跟踪 (OwnershipEnv) 和 IR opcode 已实现，但 double-move、use-after-move 的运行时检测尚未强化。 | 编译期有，运行时弱 | 1d |

## P2 — 增强

| # | 任务 | 当前状态 | 预估 |
|---|------|:--------:|:----:|
| 11 | **增量类型检查** — `typecheck-current` 全量遍历，无增量缓存。`FlatAST::dirty_` 字段存在但 TypeChecker 不读。 | 全量遍历 | 2d |
| 12 | **`--inspect` 扩展** — typecheck、evaluator、pretty JSON、组合 cache-open | 基本功能 | 1-2d |
| 13 | **Serve 超时熔断** — 当前 serve 模式缺全局超时熔断，长任务可能挂死。 | 只有 tcp-connect 有 | 1d |
| 14 | **M3 P2996 反射** — `auto_serialize<T>()` 对嵌套 struct/enum 支持有限。容器序列化部分完成。 | Phase 1-3 完成 | 持续 |
| 15 | **Benchmark 类型系统任务提分** — 15 个类型相关任务平均通过率 ~60%，低于基础任务。需分析是 hint 不足还是编译器诊断不够。 | 类型任务 60% | 1-2d |
| 16 | **`(: name Type val)` 三参数形式** — 当前 parser 只认 `(: name Type)`，`(: x Int 42)` 的 val 被当父表达式参数。已绕道用 `(check ... : Type)`，但长期应扩展 parser。 | 已绕道 | 1d |

## P3 — 中远期

| # | 任务 | 说明 |
|---|------|------|
| 17 | **蚁群控制器 Aura 级** — 当前蚁群搜索是 Python 级（字符串替换 + subprocess），应 EDSL 化：`set-code + mutate:* + eval-current` 管道，`lib/std/ant.aura` 纯 Aura 搜索循环。每变体成本 20ms → <1ms。 | [设计](design/ant_colony_controller.md) |
| 18 | **自举** — Aura 编译器用 Aura 写。等类型系统稳定后再启。 | 中型项目 |
| 19 | **多意图协作与意图树** — Phase 4：多意图协作，意图树。远期。 | — |

---

## Benchmark 基线（85 任务，max-attempts=3，1 轮）

| 模型 | 通过率 | 耗时 | 短板 |
|:----|:------:|:----:|:-----|
| Grok 4.3 | **77/85 (90.6%)** | ~17min | typesystem (8/15)、algorithm (2 个 #<procedure>) |
| MiniMax M2.7 | **74/85 (87.1%)** | ~29min | API 不稳定、有时生成 Clojure、typesystem |
| DeepSeek v4 Flash | **72/85 (84.7%)** | ~54min | typesystem (6/15)、FFI、`#<procedure>`、eval_flat 崩溃 |

**三模型共享失败**：binary-search（#<procedure>）、merge-sort（#<procedure>）、type-annot-chain、type-blame-runtime。
前两个一条 hint 可解决，后两个需编译器修复。

## 已知问题

详见 [known_issues.md](known_issues.md) — 14 个开放问题（P1:4, P2:6, P3:4）。
