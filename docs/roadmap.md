# Aura — 路线图（TODO）

**更新：2026-05-23**

**当前定位**：语言核心完备（Tree-walker + IR + JIT、Sound Gradual Typing、ADT、Linear Ownership），
已进入「让 AI 自治编辑更可靠」的迭代阶段。

**当前基准**：Grok 77/85（90.6%）、MiniMax 74/85（87.1%）、DeepSeek 72/85（84.7%）。
新增的 28 个类型系统任务（15 个）是主要拉分项，平均通过率 ~60%。

---

## ✅ 全部修复（2026-05-23）

### P0 — 编译器缺陷

| # | 问题 | Commit |
|---|------|:------:|
| 1 | Blame：`(+ 1 "hello")` 输出 stderr 错误 | `052cb19` |
| 2 | `eval_flat` 缺失 Linear/Move/Borrow/Drop 节点 | `50208da` |
| 3 | `(: x Int)` 无绑定正确报错 | `82dfaf4` |
| 4 | `(: name Type val)` 三参数解析 | `afe96fd` |
| 5 | `#<procedure>` 系统 prompt 警示 + hint 强化 | `f8166c7` |

### P1 — 功能缺口

| # | 问题 | Commit |
|---|------|:------:|
| 6 | 模块 import 类型签名（28 个 stdlib 模块 90+ 签名） | `fab6a13` |
| 7 | `let` 泛化（synthesize 路径已验证正常工作） | — |
| 8 | match 穷尽性检查（parser 元数据 + 构造器表 + type checker + 测试） | `de2c59d` |
| 9 | 树遍历器 CastOp 覆盖率 + blame 覆盖 | `2b24586` |
| 10 | M4 运行时违规检测（double-move / use-after-move / double-drop） | `6e3f78f` |

### P2 — 增强

| # | 问题 | Commit |
|---|------|:------:|
| 11 | 增量类型检查（已验证已实现） | — |
| 12 | `--inspect` 扩展（ir/closures/cache/typecheck/evaluator/pretty/cache-open） | `03be5f1` |
| 13 | Serve 超时熔断（30s async timeout） | `e259288` |
| 14 | M3 P2996 反射（P1306 递归序列化，支持嵌套 struct / 泛型 vector / array / enum） | `b07b5c6` |
| 15 | `(: name Type val)` parser 修复 | `afe96fd` |

---

## 🔄 开放 TODO

### P2

| # | 任务 | 预估 | 说明 |
|---|------|:----:|:-----|
| 16 | **Benchmark 类型系统任务提分** — 15 个类型相关任务平均通过率 ~60%。编译器缺陷已修，但需回测验证 DeepSeek 分数是否提升。也可能需优化 task hint 质量。 | 1d | 回测 + hint 迭代 |

### P3 — 中远期

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
后两个通过编译器修复已解决，前两个通过 task hint 已补充。

## 已知问题

详见 [known_issues.md](known_issues.md) — 当前无开放 issue。
