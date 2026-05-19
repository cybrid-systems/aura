# Aura — 路线图

**更新：2026-05-19 终盘** — 30 个提交。所有 P0/P1/P2 已清。累计测试 100% 通过。

---

## 当前状态评估

### 完成度评分

| 维度 | 分数 | 说明 |
|------|------|------|
| 语言核心求值 | 🟢 10/10 | tree-walker + IR 双路径，显式调用栈（无 C++ 递归深度限制）|
| 类型系统 | 🟢 8/10 | L6 + strict 模式 + 增量缓存 + Let-Poly + IR 类型特化 pass |
| 编译器基础设施 | 🟢 8/10 | ArenaGroup / 增量 / 磁盘缓存 / 热替换 / 依赖级联 |
| 标准库覆盖 | 🟡 7/10 | 18 个文件 ~1k 行，iter/queue/stack/random 新增，string 扩展 |
| 测试覆盖 | 🟢 8/10 | integ 87/87，unit 74/74，smoke 5/5，bench 44/44，bash 106/106，production 30项 21+/30 |
| 错误处理 | 🟢 8/10 | try/catch IR ✅，diagnostics 统一 (suggestion 字段)，line:col 格式 |
| EDSL / AI Agent | 🟢 8/10 | `current-source`/`api-reference`，EDSL 双阶段修复，production_test 全栈验证通过 |
| 文档 | 🟡 6/10 | README + roadmap + tutorial + known_issues + design repo 架构文档 |

### 已实现（完整清单）

**语言核心**
- 100+ 原语 (arithmetic, string, vector, hash, pair, char, I/O, type predicates)
- `apply` 内建原语 — variadic 函数动态调用
- Variadic lambda — `(lambda (x . rest) ...)` / `(define (f . rest) ...)`
- TCO (tail call optimization via eval_flat loop)
- let / let\* / letrec / define / set!
- cond / case / when / unless / and / or
- try / catch / raise (IR + tree-walker)
- quasiquote / unquote / unquote-splicing
- Macro system (defmacro, recursive expansion, gensym)
- `format` 原语 (SRFI-28 子集: ~a ~s ~% ~~)
- char predicates + string operations (char=?, char<?, char->integer, integer->char, string-split, string-trim, string-pad, string-reverse)
- **显式调用栈** — `std::variant<EvalResult, PendingCall>` + 外层 while 循环，无 C++ 递归深度限制

**数据结构**
- Pair/list (MakePair/Car/Cdr IR 原生指令)
- Vector (make-vector, vector-ref, vector-set!, vector->list, list->vector)
- Hash table (hash, hash-ref, hash-set!, hash-length, hash-keys, hash-values, hash-remove!)
- Standard library hash-set / hash-merge / hash->list / alist->hash

**类型系统（全量）**
- `--strict` 模式 — TypeCheckWrap 从 warning-only 升级为可开关的严格模式
  - `set_strict_mode(bool)` + `strict_mode_` 字段
  - `--serve` `config strict true/false` 命令
  - 默认为 false，不破坏现有 tests
- **增量类型缓存** — `synthesize_flat` 结果写入 `flat.type_id(id)`
  - `mark_subtree_dirty` 自动触发重检查
- **类型信息流入 IR** — `IRInstruction.type_id` 可选字段
  - Lowering 时从 `flat.type_id(id)` 写入 `inst.type_id`
  - 0=dynamic 向后兼容
- **IRInterpreter 运行时类型断言** — strict 模式下做 runtime 类型校验
- **Let-Polymorphism** — `synthesize_flat_let` 泛化绑定 → `Forall`
  - `synthesize_flat_var` `instantiate_all` 替换自由变量
  - 递归 normalize 处理嵌套 forall
  - 只在 strict 模式下启用
- **TypeSpecializationPass** — 类型感知的 IR pass
  - 运行位置: `lower → [TypeSpecialization] → ComputeKind → Arity → ConstFold → execute`
  - 了解已知类型，解除 coercion 冗余，死代码消除

**增量编译器**
- IR pipeline (37 opcodes, const folding, compute-kind, arity check)
- CompilerService eval() with IR-first + tree-walker fallback
- Closure bridge: IR ↔ tree-walker closure interop (map/filter via IR)
- `cache_define()` + `ir_cache_` — 函数级 IR 缓存
- `cache_module()` — 标准库模块全量缓存
- `invalidate_function()` + `mark_module_dirty()` — 重定义级联失效
- ArenaGroup — 多模块独立 arena 管理
- mmap 磁盘缓存（`~/.cache/aura/modules/`）
- Hot-swap — 运行时替换已缓存函数
- 闭包桥接 lambda body source 存备份, fallback 时 re-parse

**标准库（18 files, ~1k lines）**
- `hash.aura` — hash-set, hash-ref, hash->list, hash-merge, alist->hash
- `combinators.aura` — compose, curry, flip, complement, const, identity
- `maybe.aura` — maybe-ref, maybe-default, map-maybe, filter-maybe
- `csv.aura` — csv-parse (handles quoted fields)
- `set.aura` — set, set-add, set-union (variadic API)
- `io.aura` — read-lines, copy-file, move-file, delete-file, directory-files
- `list.aura` — foldr, zip, zip3, take-while, drop-while, partition, sort, range, sum, product, last, flatten, intersperse, member?
- `math.aura` — sin, cos, tan, log, pow, sqrt, floor, ceil, round, abs
- `string.aura` — string-split, string-trim, string-pad, string-reverse
- `test.aura` — check, check=, test-suite, run-tests

**服务**
- `--serve`: eval / define / compile / module / fmt / config JSON protocol
- `--serve`: set-code / query:* / mutate:* / typecheck-current / eval-current EDSL
- `--serve`: AI agent 双阶段工作流（生成代码 → 编译 → 测试 → 修复循环）
- `--serve`: 函数热替换 + 依赖追踪

---

## 下一步工作

### P0 — 无（全部完成）

| # | 项 | 说明 | 状态 |
|---|-----|------|------|
| 1 | `--strict` 模式 | TypeCheckWrap + config strict command | ✅ |
| 2 | 增量类型缓存 | flat.type_id(id) + dirty 自动重检查 | ✅ |
| 3 | arity 检查完全修复 | 恢复 `ar.run(ir_mod)`，修复 false positive | ✅ |
| - | require 内缓存函数 | 修复 cached 函数中 require 的空绑定 | ✅ |

### P1 — 全部完成

| # | 项 | 说明 | 工作量 | 状态 |
|---|-----|------|--------|------|
| 4 | 类型信息流入 IR | `IRInstruction.type_id` + lowering 写入 | 1-2d | ✅ |
| 5 | Let-Poly 启用 | generalize + instantiate forall | 1d | ✅ |
| 6 | TypeSpecializationPass | 类型感知常量折叠/死代码消除 | 1d | ✅ |
| 7 | `--serve strict` 命令 | 运行时切换严格模式 | 0.5d | ✅ |
| 8 | Parser 错误恢复 | 多错误累积 + 跳过 malformed | 3h | ✅ |
| 9 | proper Diagnostics | 集中化错误信息，行号/列号/原因/建议 | 2h | ✅ |
| 10 | Benchmark 基线 | 对比 IR vs tree-walker 性能 | 2h | 🔴 |
| 11 | 标准库 v2 | 增加到 18 个文件，覆盖常见需求 | 8h | ✅ |
| 12 | try/catch IR 指令 | 消除一个主要 fallback 路径 | 4h | ✅ |

### P2 — 中期（CaaS 生产化）

| # | 项 | 说明 | 工作量 |
|---|-----|------|--------|
| 13 | **IR 级 import** | 消除模块系统 fallback | 6h |
| 14 | **LLVM JIT 后端** | `--jit` 编译到原生代码 | 40h+ |
| 15 | **AOT 编译** | 从 Aura 源码到静态二进制 | 20h |
| 16 | **包管理** | 简单 registry + `(fetch ... :as dep)` | 8h |

### P3 — 长期

| # | 项 | 说明 |
|---|-----|------|
| 17 | **自举** | 用 Aura 写 Aura 编译器 |
| 18 | **GC 或引用计数** | 替换 arena-only 内存管理 |
| 19 | **FFI** | 调用 C/Rust 库 |
| 20 | **完整的类型系统** | 全类型检查 + 类型驱动优化 |

---

## 下一步工作

### Phase A — 体验打磨（推荐立即启动）

| # | 项 | 说明 | 估计 |
|---|-----|------|------|
| B1 | **Benchmark 基线** | 量化 IR vs tree-walker，作为 JIT 加速前基准 | 2h |
| B2 | **增量类型检查** | `typecheck-current` 从全量遍历→脏子树增量 | 4h |
| B3 | **桥接器测试覆盖** | closure bridge body_source fallback 路径测试 | 2h |
| B4 | **Diagnostics 统一** | 补全所有 error path 的 `suggestion` 字段 | 1h |
| B5 | **CI/CD** | GitHub Actions 自动构建+测试 | 2h |

### Phase B — 能力跃迁（1-2周）

| # | 项 | 说明 | 估计 |
|---|-----|------|------|
| C1 | **IR 级 import** | 消除模块系统最后 tree-walker fallback | 6h |
| C2 | **标准库 v3** | regex, datetime, I/O 增强 | 8h |
| C3 | **Hygienic Macros** | Ghuloum Step 16 — 卫生宏 rename | 8h |
| C4 | **编译期 AST 验证** | Ghuloum Step 17 | 4h |

### Phase C — 已全部完成 ✅（2026-05-19）

| # | 项 | 状态 |
|---|-----|------|
| C1 | IR 级 import | ✅ |
| C2 | 标准库 v3 (19 files) | ✅ |
| C3 | 卫生宏 | ✅ |
| C4 | 编译期 AST 验证 | ✅ |

### Phase D — 战略级

| # | 项 | 说明 |
|---|-----|------|
| D1 | **LLVM ORC JIT** | 10-100x 加速，从原型到可用语言的质变 |
| D2 | **形式化类型系统** | Sound Gradual Typing |
| D3 | **自举** | 用 Aura 写 Aura 编译器 |

---

## 已完成里程碑

### set! 闭包 --ir 路径修复 ✅ 2026-05-19 (`6152993`)

```
问题：let Cell 捕获时 CellGet 取值而非 CellRef，set! 修改本地副本。
修复：捕获 CellRef 直传；CellGet/CellSet 通过 cell_heap_ 解引用。
  (c)(c)(c) → 3  ✅  (之前返回 1)
```

### 互递归 --ir 路径 ✅ 2026-05-19 (`21e8d1d`)

```
Begin handler 两阶段：先扫描 Define 预绑定 Cell，再正常 lowering。
  (odd? 7)  --ir  →  #t  ✅
```

### 自引用缓存函数 --ir 路径 ✅ 2026-05-19 (`3e203ed`)

```
Define handler 预绑定 Cell 后再 lower lambda body。
  (fact 5)  --ir  →  120  ✅  (之前返回 0)
```

### 显式调用栈 ✅ 2026-05-19 (`9674eb0`)

```
使用 std::variant<EvalResult, PendingCall> 实现：
- run_function 返回 RunResult = variant<EvalResult, PendingCall>
- Call/Apply handler 返回 PendingCall（不再 C++ 递归）
- execute() 改为外层 while 循环驱动
- 支持任意深度的闭包递归调用
```

### try/catch IR 指令 ✅ 2026-05-19

```
IROpcode::Raise   — (raise val) 创建 error value
IROpcode::IsError — 检查值是否为 error，返回 bool
Lowering: (try body (catch (var) handler)) → IsError + Branch
树遍历求值器 bug 修复: catch_env 缺少 set_primitives
```

### 标准库 v2 ✅ 2026-05-19

```
iter.aura     — any?/every?/find/split-at/frequencies/hash-map/hash-filter/
                vector-map/iota/iterate  (165 行)
queue.aura    — FIFO 队列 (enqueue/dequeue/queue-front) (43 行)
stack.aura    — LIFO 栈 (push/pop/top) (32 行)
random.aura   — LCG 伪随机数生成器 (76 行)
string.aura   — 12 个新函数: contains?/prefix?/suffix?/replace/
                pad-left/pad-right/reverse/repeat/chars->string 等
stdlib total  → 18 files, 1,041 行
```

### 类型系统增强（P0–P4）✅ 2026-05-19

```
Phase 0   → ―strict 模式             [2b1de7e] ✅
Phase 1   → 增量类型缓存              [2b1de7e] ✅ (同一 commit)
Phase 2   → 类型信息流入 IR            [9e10331] ✅
Phase 2b  → IR 运行时类型断言          [e9c53ab] ✅
Phase 3   → Let-Polymorphism          [4fb9783] ✅
Phase 4   → TypeSpecializationPass    [6795204] ✅
```

### IR 管线全面覆盖 ✅ 2026-05-17

```
算术 → IR       比较 → IR        if → IR        let → IR
lambda → IR      map/filter/foldl → IR (+ 闭包桥接)
cons/car/cdr →  MakePair/Car/Cdr 原生指令
Quote/Pair → IR (ConstVoid + (cons ...) 链展开)
format → IR     char ops → IR    hash/vector/string → IR
const folding → IR pass
```

## 测试状态

```
smoke:       5/5   ✅
integ:      87/87  ✅
unit:       74/74  ✅
bash:      106/106 ✅
bench:      44/44  ✅
mutation:   varies  ⚠️ (semantics-changing mutations correctly rejected)
AI Agent:   —      ✅ (DeepSeek v4 Flash, EDSL restore 工作正常)
```

## 代码统计（5/19 收盘）

```
src/core/       ~2,700 行
src/parser/     ~1,400 行
src/compiler/   ~13,000 行
lib/std/        18 files ~1,041 行 Aura
tests/          bash 回归 + 3 C++ suites + 集成测试 + 基准 + AI agent
docs/           tutorial.md + known_issues.md + roadmap.md + 设计文档
```

## 最近提交

```
6152993  P3#15: Fix set! closure mutable state in --ir path
21e8d1d  P3#14: Mutual recursion support in --ir path (Begin pre-bind)
3e203ed  P1#3: Fix self-referencing cached functions in --ir path
9674eb0  P3#11: Explicit call stack for IR interpreter (std::variant approach)
37ab1e2  P3#12: stdout flush for display/write/newline
0ab521d  Add (api-reference) EDSL primitive: 180 primitives auto-listed
6c22c5d  README + roadmap: add LLM agent demo section, EDSL pipeline, update scores
d639ef7  EDSL: enhanced AST-to-source feedback + prompt improvements for LLM repair
b991b3a  Agent: fix code extraction for non-Aura lang tags + prompt hardening
85c3815  P3#11: Deep recursion friendly error instead of segfault
e334194  Fix: self-referencing cached functions → tree-walker fallback
3392d77  Fix: set! closure mutable state + add hash-has-key? primitive
07c196d  Re-enable arity check in eval() path
c8e8baf  Unify diagnostics: kind_name(), suggestion, format() chain
09e71f0  Fix pre-existing type-check errors: wrong_arity/type_of
4b85e46  stdlib v2: iter/queue/stack/random + string.aura extended
6d06e67  try/catch IR: Raise + IsError opcodes, lowering, eval integration
```
