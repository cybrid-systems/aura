# Aura — 路线图

**更新：2026-05-19** — 类型系统增强计划（P0–P4）全部完成。全量类型信息流入 IR 管线。

---

## 当前状态评估

### 完成度评分

| 维度 | 分数 | 说明 |
|------|------|------|
| 语言核心求值 | 🟢 9/10 | tree-walker + IR 双路径稳定，IR 桥接器修复，pair 原生指令，TCO，format |
| 类型系统 | 🟢 8/10 | L6 + strict 模式 + 增量缓存 + Let-Poly + IR 类型特化 pass |
| 编译器基础设施 | 🟢 8/10 | ArenaGroup / 增量 / 磁盘缓存 / 热替换 / 依赖级联 |
| 标准库覆盖 | 🟡 7/10 | 18 个文件 ~1k 行，iter/queue/stack/random 新增，string 扩展 |
| 测试覆盖 | 🟡 6/10 | integ 87/87，unit 74/74，smoke 5/5，bench 44/44，bash 106/106 |
| 错误处理 | 🟡 6/10 | Parser 多错误累积 + line:column，try/catch IR ✅，Diagnostics 统一 ✅ |
| EDSL / AI Agent | 🟢 7/10 | `current-source` AST→source 桥接，LLM 管线实测通过，set!闭包修复 |
| 文档 | 🟡 6/10 | README + roadmap + tutorial + known_issues + 设计文档 |

### 已实现（完整清单）

**语言核心**
- 100+ 原语 (arithmetic, string, vector, hash, pair, char, I/O, type predicates)
- `apply` 内建原语 — variadic 函数动态调用
- Variadic lambda — `(lambda (x . rest) ...)` / `(define (f . rest) ...)`
- TCO (tail call optimization via eval_flat loop)
- let / let\* / letrec / define / set!
- cond / case / when / unless / and / or
- try / catch / raise (仅 tree-walker，无 IR 指令)
- quasiquote / unquote / unquote-splicing
- Macro system (defmacro, recursive expansion, gensym)
- `format` 原语 (SRFI-28 子集: ~a ~s ~% ~~)
- char predicates + string operations (char=?, char<?, char->integer, integer->char, string-split, string-trim, string-pad, string-reverse)

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

### P0 — 立即（补齐功能断点，让语言可写 200 行脚本）

| # | 项 | 说明 | 工作量 | 状态 |
|---|-----|------|--------|------|
| 1 | **`--strict` 模式** | TypeCheckWrap + config strict command | 2h | ✅ |
| 2 | **增量类型缓存** | flat.type_id(id) + dirty 自动重检查 | 2h | ✅ |
| 3 | **arity 检查完全修复** | 恢复 `ar.run(ir_mod)`，修复 `resolve_callee` Primitive 误报 | 3h | ✅ |
| - | **require 内缓存函数** | 修复 cached 函数中 require 的空绑定 | 2h | ✅ |

### P1 — 短期

#### 类型系统增强

| # | 项 | 说明 | 工作量 | 状态 |
|---|-----|------|--------|------|
| 4 | **类型信息流入 IR** | `IRInstruction.type_id` + lowering 写入 | 1-2d | ✅ |
| 5 | **Let-Poly 启用** | generalize + instantiate forall | 1d | ✅ |
| 6 | **TypeSpecializationPass** | 类型感知常量折叠/死代码消除 | 1d | ✅ |
| 7 | **`--serve strict` 命令** | 运行时切换严格模式 | 0.5d | ✅ |

#### 基础设施

| # | 项 | 说明 | 工作量 | 状态 |
|---|-----|------|--------|------|
| 8 | **Parser 错误恢复** | 多错误累积 + 跳过 malformed | 3h | ✅ |
| 9 | **proper Diagnostics** | 集中化错误信息，行号/列号/原因/建议 | 2h | 🔴 |
| 10 | **Benchmark 基线** | 对比 IR vs tree-walker 性能 | 2h | 🔴 |
| 11 | **标准库 v2** | 增加到 18-20 个文件，覆盖常见需求 | 8h | ✅ |
| 12 | **try/catch IR 指令** | 消除一个主要 fallback 路径 | 4h | ✅ |

### P2 — 中期（CaaS 生产化）

| # | 项 | 说明 | 工作量 |
|---|-----|------|--------|
| 13 | **IR 级 import** | 消除模块系统 fallback | 6h |
| 14 | **LLVM JIT 后端** | `--jit` 编译到原生代码 | 40h+ |
| 15 | **AOT 编译** | 从 Aura 源码到静态二进制 | 20h |
| 16 | **包管理** | 简单 registry + `(fetch "..." :as dep)` | 8h |

### P3 — 长期

| # | 项 | 说明 |
|---|-----|------|
| 17 | **自举** | 用 Aura 写 Aura 编译器 |
| 18 | **GC 或引用计数** | 替换 arena-only 内存管理 |
| 19 | **FFI** | 调用 C/Rust 库 |
| 20 | **完整的类型系统** | 全类型检查 + 类型驱动优化 |

---

## 已完成里程碑

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
unit:       74/74  ✅ (含 TypeChecker 8 新增)
bash:      106/106 ✅
bench:      44/44  ✅
mutation:   varies  ⚠️ (TypeSpecialization 改变常量折叠行为)
AI Agent:   —      ✅ (DeepSeek v4 Flash, EDSL restore 工作正常)
```

## 代码统计（5/19 收盘）

```
src/core/       ~2,700 行
src/parser/     ~1,400 行
src/compiler/   ~12,800 行
lib/std/        18 files ~1,041 行 Aura
tests/          bash 回归 + 3 C++ suites + 集成测试 + 基准 + AI agent
docs/           tutorial.md + known_issues.md + roadmap.md + 设计文档
```

## 最近提交

```
6795204  Phase 4: TypeSpecializationWrap — type-aware IR pass
4fb9783  Phase 3: Let-Polymorphism (generalize + instantiate + recursive normalize)
e9c53ab  Phase 2b: IRInterpreter runtime type assertions (strict mode)
9e10331  Phase 2: type information flows into IR (IRInstruction.type_id)
2b1de7e  P0: strict mode for type checker + serve config command
3cb9a33  Fix recursive function IR caching
6dfbfb4  Refresh docs after today's fixes
02681ac  Bridge: save lambda body source for fallback re-parse
1a9b261  Fix require inside cached functions + disable arity false positive
e5335d1  Arity diagnostic: add caller name to mismatch message
cdf6cc9  stdlib: export map, for-each, member? from list.aura
e4705a8  Update known_issues.md + fix agent empty-response infinite loop
37f1361  Fix cached function IR issues: string pool, func refs, bridge data
af04a12  Fix IR closure env capture and closure ID collision
dbb961c  Fix parse_lambda: handle multiple body expressions
5ba86d1  Parser error recovery: skip malformed expressions and continue
0c16afb  Fix unit tests: comparison returns #t/#f, define scope in lowering
65f2dbe  Add tutorial.md — 10-minute quickstart
ca6a7d0  Add integration tests: apply, variadic, char, string ops, format
b2ae103  Add format primitive (SRFI-28 subset: ~a ~s ~% ~~)
e36895d  Add char predicates + string operations (10 primitives)
```
