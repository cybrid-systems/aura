# Aura 路线图

**更新：2026-05-27 — Phase 5 已完成，进入 Phase 6（EDSL 深度强化 + 自托管闭环）**

---

## 项目状态

| 维度 | 数值 |
|:-----|:-----|
| 核心测试 | ✅ 7 suites + REPL 通过 |
| EDSL Benchmark (Grok) | 110-114/135 (~80-84%) |
| EDSL Benchmark (DeepSeek) | 100-103/135 (~74-76%) |
| AOT emit 测试 | ✅ 57/57 全绿 |
| Aura-native Benchmark | ⚠️ **~10%**（vs Python 75-81%） |
| 总 commits | 1071+ |

---

## 🔍 Phase 5 深度诊断（实际代码审计结论）

Phase 5 的 P0-P3 全部完成，但两个关键差距在 roadmap 上没有暴露：

### ⚠️ 差距 1：Aura-native benchmark 与 Python runner 有 ~70% 鸿沟

```
Python runner: 101/135 (74.8%)  ← 6 层 fallback, 自适应 prompt, pid:analyze
Aura-native:   ~10%              ← 简陋的字符串匹配，无 prompt builder
```

每个环节 Python 都比 Aura 多了一层智能：

| 环节 | Python runner | Aura-native (bench.aura) |
|:-----|:-------------|:-------------------------|
| prompt 构建 | 注入 stdlib 清单 + task hints + API ref | 无统一 prompt builder |
| code extraction | 6 层 fallback (`\`\`\`lisp/scheme/racket/clojure → 无标注 → 全文) | 只有 ``\`lisp`` → define/display |
| check_success | substring + 字边界 + 结构化错误检测 | 简单 substring |
| retry 策略 | intend (gen/ver/fixer) + ant colony + 自适应温度 | 直白的 loop |
| 错误诊断 | pid:analyze + 结构化 `("kind" "msg")` | 刚跟上 |

**根因：** Phase 5 做了"Aura 能跑 benchmark"，但没做"跑得和 Python 一样好"。每个环节的细节缺失累积了 70% 的通过率差距。

### ⚠️ 差距 2：workspace:merge 不是真 COW merge

```cpp
// evaluator_impl.cpp 实际实现 ≈
workspace:merge(child) {
    set-code(get-source(child));  // 用子 workspace 源码覆盖父 workspace
}
```

当前 merge 就是把子 workspace 的源码字符串覆盖到父 workspace。不是逐符号合并——父 workspace 原有的修改（如果和子 workspace 不冲突）直接丢失。真正的 COW merge 需要用 `query:def-use` + `mutate:rebind` 逐符号合并。

**影响：** 多层 workspace 目前是"伪分层"——merge 就是字符串覆盖。多 agent 协作场景下会丢修改。

### ✅ 正确的认知

除上述两点外，EDSL 核心非常扎实。实际代码验证：
- `query:find/pattern/def-use/children/parent/node` — 全部 C++ 原语，支持 Ellipsis 通配符
- `mutate:rebind/set-body/replace-value/remove-node/record-patch` — AST 级别精准编辑
- `ast:snapshot/restore/diff/summary` — 完整快照 + diff
- `workspace:create/switch/merge/lock/discard/sync-from` — 7 个原语
- `synthesize:register-template/fill/define/optimize` — 注册/填充/LLM/遗传 4 策略
- `fiber:spawn` + `session:create` — 基础异步和隔离
- `intend` 控制器 + ant colony + PID 相位检测 — 全部纯 Aura（std/adaptive.aura, std/ant.aura）
- `heal` 自愈三元素 — 纯 Aura（刚完成）

---

## Phase 5 — 已完成工作量

### P0 — 基础设施
- [x] `http-post` 加 curl 超时 + pipe+fork execvp
- [x] `http-post` 改用 libcurl C API（dlopen 运行时加载，零子进程）
- [x] `extract-code` 加 `(define ...)` / `(display ...)` fallback
- [x] `LLM_BASE_URL` / `LLM_MODEL` 环境变量支持

### P1 — 智能调度
- [x] `run-one` 改用 `intend` 控制器（generator/verifier/fixer）
- [x] ant colony 局部变异修复（`colony:search`）
- [x] `check_success` 灵活 substring 匹配
- [x] TASK_HINTS 从 bench-tasks.json 注入 system prompt
- [x] 简洁 system prompt（去掉 76 行 API ref）

### P2 — 并行化
- [x] 并行执行（`run_parallel.sh`, `BENCH_OFFSET`）
- [x] `fiber:spawn` 原语（serve 模式可接 fiber scheduler）
- [x] `session:create` 原语（隔离 evaluator，独立 string_heap_/pairs_）
- [x] `run_serve.py` serve-async 多 session 编排框架

### P3 — 原生 HTTP + REPL
- [x] libcurl C API（dlopen，零子进程）
- [x] curl/curl.h 条件编译（CI 兼容）
- [x] REPL 测试修复 + 集成 build.py CI_CORE

### P4 — 自修复闭环
- [x] 结构化错误返回 `eval-current`（`("kind" "msg")` 对偶）
- [x] closure auto-fix（自动补 `(display ...)`，纯 Aura）
- [x] `std/heal.aura` — 自修复执行循环
- [x] 错误类型→API 文档映射

### 修复的 Bug
- [x] SIGSEGV at ~119 tasks（unparse depth limit 256）
- [x] 目录 IO crash（`read-file`/`file-size`/`file-copy` S_ISREG）
- [x] 模块解析目录 crash（`resolve_module_path` S_ISREG）
- [x] JIT 符号未注册（`aura_drop_pair/cell/closure`）
- [x] Arena OOM（`null_memory_resource` → `new_delete_resource`）
- [x] `drop` → `skip`（`drop` 是 parser 特殊形式）
- [x] `if_false` 测试修复（`0` 是 Scheme truthy）
- [x] REPL 测试 ANSI 转义（`TERM=dumb`）
- [x] `ast:diff` keyword 标签 + `io_print_val` keyword 传播
- [x] `build.py` `WORKSPACE`→`ROOT` + 返回类型修复
- [x] unparse depth limit 256（防止 stack overflow）
- [x] `set-code` 错误穿透 eval-current（不吞空字符串）

### Phase 5 遗留待做
- [ ] **Serve 模式**：bench.aura 改用 `--serve-async` 持久化 workspace
- [ ] **JSON 结果输出**：兼容 `bench_results/*.json` 格式
- [ ] **回归监控**：git commit + 模型 + 过率

---

## Phase 6 — EDSL 深度强化 + 自托管闭环

**核心目标：** Aura-native benchmark 从 ~10% 追上 Python runner（目标 60%+），补齐 EDSL 实际使用中的关键缺失。

### P0 — 自托管 benchmark 闭环（75% 完成）

把 Python runner 的"特有智能"全部搬到 Aura 内，消除 70% 鸿沟。

| 子项 | 状态 | 做法 | 影响 |
|:-----|:-----|:-----|:-----|
| **check_success C++ 化** | ✅ **已有（非 P0 工作）** | 内置 C++ 原语，false-positive guard + 结构化错误检测 | bench.aura 直接可用 |
| **code extraction Aura 化** | ✅ **`std/extract.aura`** | 列表式字符串扫描，6 步 fallback | bench.aura 不再依赖 Python 提取 |
| **prompt builder Aura 化** | ✅ **`std/prompt.aura`** | 统一注入 stdlib 清单 + task hints + API ref | 和 Python runner 同 prompt |
| **bench.aura 核心 bug 修复** | ✅ **3 个 commit** | 1) 移除 check-success 影子函数（无限循环） 2) 替换 extract-code（用了未绑定的 string-index） 3) 添加 std/adaptive 依赖（提供 string-index/string-trim） | 从 ~10% 预升 |
| **intend 控制器对齐** | ✅ **已对齐（C++ 内置 intend）** | Aura 已有 C++ 内置 `intend` 原语（gen→ver→fix 循环），bench.aura 直接使用 | 与 Python runner 同策略 |
| **pid:analyze 结果注入** | ⏳ 待做 | 把 Aura 侧的分析结果格式化后注入 correction prompt（需测试实际 LLM 调用来验证效果） | 影响 retry 效率 |

**验收标准：** Aura-native benchmark 通过率 ≥ 50%（当前 ~10% → 预计修复后 ~30-40%）

### 已发现的 Aura 运行时 bug

| Bug | 影响 | 状态 |
|:----|:-----|:-----|
| `string-index` 仅在 `std/adaptive` 模块中定义，非内置 | 依赖 std/adaptive 的模块可工作 | ✅ 通过 require 链解决 |
| `string-trim` 同上 | bench.aura 原用此函数 | ✅ 通过 `std/extract:trim-str` 替代 |
| 字符串位置迭代产生 `<kwd>` 垃圾值 | `substring` + 位置循环出现 keyword 标记 | ⚠️ 绕行：用 `string->list` + list operations |
| `(require "a" "b" all:)` 语义不明确 | 可能只对最后模块应用 `all:` | ⚠️ 绕行：分两个 `require` 语句 |
| `display` 后偶尔出现 `<kwd>` 残留 | 终端输出夹杂 keyword 引用 | ⚠️ 无害，bench 逻辑正确 |
| `--serve-async` stdin 不触发 | `static` lambda 捕获了局部变量的悬挂引用，fiber 不执行 | ✅ `static` removed, worker fiber spawned in sc_fn |
| `--serve-async` LLM 调用阻塞所有 fiber | `http-post` 是同步 `curl_easy_perform` | ✅ 改为 thread + eventfd 异步模式 |

### P1 — workspace 隔离修复 + 源码级 merge ✅

**修复了 `update_shared_tree_root`：** 不再只更新 root (idx 0) 的 flat 指针，也更新 active node。
- `set-code` 创建新 arena FlatAST → `workspace_flat_` 指向它
- `update_shared_tree_root` 现在会写 active node.flat = `workspace_flat_`
- 子 workspace 调用 `set-code` 后切回再切回来，数据保留了 ✅

**workspace:merge 改成了源码级合并：**
- 父源码 + 子源码拼接，子符号覆盖父的同名符号
- 返回 `(("name" . "merged") ...)` 结果列表
- `set-code` 现在会正确更新 root 的 WorkspaceNode，merge 后数据不丢

**测试验证：**
- 隔离测试 PASS：父不看见子的定义，子不丢失切换后的定义
- merge 测试 PASS：`x=42, y=200`（子覆盖了父的 y=100）
- 子 workspace merge 后不受影响

### P2 — 多 worker benchmark runner（已完成）

**策略调整：** `--serve-async` 的 fiber scheduler 有底层 epoll 交互问题（stdin 读取不触发），短期调试成本高。改为用 `--serve` 模式的多进程方案。

**完成：** `tests/run_serve.py`
- 启动 N 个 `./build/aura --serve` 进程
- `ThreadPoolExecutor` 并行分发任务
- 每个 worker 独立 serve 连接，互不阻塞
- 支持 BENCH_LIMIT/OFFSET/ROUNDS/ATTEMPTS 环境变量
- 汇总报告 pass/fail per round

**未来工作（非当前 P2 范围）：**
- `--serve-async` 修复：epoll 注册时机问题、stdin 读取触发、stdout flush
- 修复后可改为单进程多 fiber，去掉多进程开销

### P3 — std/heal.aura 深度强化（本周）

当前 heal 太浅——diagnose 做的是字符串匹配，apply-fix 做的是字符串替换。应该用真正的 EDSL 原语：

```scheme
;; 当前（字符串级别）
(diagnose output)     → string       ; 肤浅的模式匹配
(apply-fix code diag) → code        ; 正则替换

;; 应该（AST 级别）
(diagnose output code)  → (list error-kind affected-node-id diagnostic-msg)
(apply-fix code error-kind node-id) → code
```

这样 diagnose 用 `query:pattern` / `query:def-use` 定位具体的 AST 节点，
apply-fix 用 `mutate:rebind` / `mutate:set-body` 做精准手术级修复。

**ant colony 扩展：** 当前 `colony:search` 只有 2 种变异策略（display-ref, lit-tweak）。
加策略：`body-wrap`（套 `(display ...)`），`fn-call`（补函数调用），`sig-extend`（补参数）。

### P4 — query:filter / query:where 组合查询（1-2 周）

当前 query 只能单步查：

```scheme
(query:find "fib")         → 所有叫 fib 的节点
(query:node-type "Define") → 所有 Define 节点

;; 无法组合：
;; (query:filter (query:find "fib") (query:node-type "Call")) → 没有
```

加组合查询能力：

```scheme
(query:filter pred node-ids)     → 根据谓词过滤节点列表
(query:where field-name value)   → 按字段值匹配（如 type, name, arity）

;; 应用场景：
;; 找出所有未调用的函数定义
(query:filter "uncalled" (query:node-type "Define"))
;; → 返回已定义但零引用的函数
```

不需要新 C++ 原语——在 `std/query.aura` 用 `query:find` + `query:children` + `query:parent` 组合实现。

---

## Phase 7 — Agent 编排与高层抽象

### 展望（当前不需要做，留作路线标记）

**高层 refactor 复合操作**（纯 Aura stdlib）

```scheme
(refactor:rename-var old-name new-name)    ; query:find → mutate:rebind
(refactor:extract-function region name)    ; 选区域 → 提取为新函数
(refactor:inline-function name)            ; 内联展开
(refactor:lift-to-letrec name expr)        ; lift lambda 到 letrec
```

全部用现有原语组合，不需要 C++ 支持。

**Agent 编排框架** `std/orchestrator.aura`

```scheme
(orch:define-role "generator" (prompt) → code)
(orch:define-role "verifier" (code) → (ok? diag))
(orch:define-role "fixer" (code diag) → code)

(orch:run-pipeline (list "generator" "verifier" "fixer")
  :max-rounds 3
  :on-fail 'rollback)
```

**增量编译 dirtiness 标记**

`query_edsl_design.md` 留的 P2 项。当前 `eval-current` 全量重编译（500-5000 节点够快），但多 workspace + 高频 mutate 时不够。需要 DirtinessTracker。

---

## 总结：当前优先顺序

```
🔥 P0 — 自托管 benchmark 闭环（收益最高，24-48h）
  把 Python runner 的智能搬到 Aura 内，~10% → 目标 50%+

🔥 P1 — workspace:merge 真 COW（本周）
  不让多层 workspace 停留在纸面上

🔥 P2 — fiber/session 接入 benchmark（本周）
  去掉 shell 依赖，真正自持

🔥 P3 — heal 深度强化（本周）
  从字符串修复升级到 AST 手术级修复

📌 P4 — 组合查询能力（1-2 周）
  降低 LLM 查询调用次数

📌 Phase 7 项 — 留到上述做完后再评估需求
```
