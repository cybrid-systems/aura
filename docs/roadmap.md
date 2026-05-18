# Aura — 路线图

**更新：2026-05-18** — 经过两轮 sprint（CaaS + 增量编译 + 语言打磨）后重新评估。

---

## 当前状态评估

### 完成度评分

| 维度 | 分数 | 说明 |
|------|------|------|
| 语言核心求值 | 🟢 8/10 | tree-walker + IR 双路径稳定，IR 桥接器修复，pair 原生指令，TCO |
| 编译器基础设施 | 🟢 8/10 | ArenaGroup / 增量 / 磁盘缓存 / 热替换 / 依赖级联 / 磁盘缓存 |
| 标准库覆盖 | 🟡 5/10 | 12 个文件 ~450 行，char/string/format 补齐，缺少 string-split |
| 测试覆盖 | 🟡 4/10 | integ 87/87，unit 61/61，smoke 5/5，仍缺 EDSL 管线测试 |
| 错误处理 | 🟡 4/10 | Parser 多错误累积 + line:column，DCE 后显示所有错误 |
| 类型系统 | 🟡 5/10 | L6 渐进类型 + occurrence typing + warnings-only 模式 |
| 文档 | 🟡 4/10 | README + roadmap + tutorial + known_issues + 设计文档 |
| AI agent 集成 | 🟡 6/10 | EDSL 管线完整，DeepSeek API 实测通过，空响应保护 |

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

**数据结构**
- Pair/list (MakePair/Car/Cdr IR 原生指令)
- Vector (make-vector, vector-ref, vector-set!, vector->list, list->vector)
- Hash table (hash, hash-ref, hash-set!, hash-length, hash-keys, hash-values, hash-remove!)
- Standard library hash-set / hash-merge / hash->list / alist->hash

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

**标准库（10 files, ~400 lines）**
- `hash.aura` — hash-set, hash-ref, hash->list, hash-merge, alist->hash
- `combinators.aura` — compose, curry, flip, complement, const, identity
- `maybe.aura` — maybe-ref, maybe-default, map-maybe, filter-maybe
- `csv.aura` — csv-parse (handles quoted fields)
- `set.aura` — set, set-add, set-union (variadic API)
- `io.aura` — read-lines, copy-file, move-file, delete-file, directory-files
- `list.aura` — foldr, zip, zip3, take-while, drop-while, partition, sort, range, sum, product, last, flatten, intersperse
- `math.aura` — sin, cos, tan, log, pow, sqrt, floor, ceil, round, abs
- `string.aura` — string-split, string-trim, string-pad, string-reverse
- `test.aura` — check, check=, test-suite, run-tests

**服务**
- `--serve`: eval / define / compile / module / fmt JSON protocol
- `--serve`: set-code / query:* / mutate:* / typecheck-current / eval-current EDSL
- `--serve`: AI agent 双阶段工作流（生成代码 → 编译 → 测试 → 修复循环）
- `--serve`: 函数热替换 + 依赖追踪

---

## 下一步工作

### P0 — 立即（补齐功能断点，让语言可写 200 行脚本）

| # | 项 | 说明 | 工作量 | 状态 |
|---|-----|------|--------|------|
| 1 | **`--strict` 模式** | `TypeCheckWrap` 从 warning-only 升级为可开关的严格模式 | 2h | ✅ 已实现 |
| 2 | **增量类型缓存** | `synthesize_flat` 结果写入 `flat.type_id(id)`，dirty 节点自动重检查 | 2h | 🔴 |
| 3 | **arity 检查完全修复** | 恢复 `eval()` 中 `ar.run(ir_mod)`，修复 `resolve_callee` Primitive 误报 | 3h | ⚡ |

### P1 — 短期（提升可靠性和体验，支持 1000 行项目）

#### 类型系统增强

| # | 项 | 说明 | 工作量 | 前置 |
|---|-----|------|--------|------|
| 4 | **类型信息流入 IR** | `IRInstruction.type_id` 可选字段 + lowering 时写入 + IRInterpreter 运行时断言 | 1-2d | P0#1 |
| 5 | **Let-Poly 启用** | `synthesize_flat_let` 泛化 + `synthesize_flat_var` 实例化 forall | 1d | P0#2 |
| 6 | **PassManager 集成** | TypeSpecializationPass — 类型感知常量折叠/死代码消除 | 1d | #4 |
| 7 | **`--serve strict` 命令** | 运行时切换严格模式，EDSL 管线实时反馈 | 0.5d | #1 |

#### 基础设施

| # | 项 | 说明 | 工作量 |
|---|-----|------|--------|
| 8 | **Parser 错误恢复** | 多错误累积（已部分实现），`(export ...)` 多模块导出链 | 3h |
| 9 | **proper Diagnostics** | 集中化错误信息，行号/列号/原因/建议 | 2h |
| 10 | **Benchmark 基线** | 对比 IR vs tree-walker 性能 | 2h |
| 11 | **标准库 v2** | 增加到 15-20 个文件，覆盖常见需求 | 8h |
| 12 | **try/catch IR 指令** | 消除一个主要 fallback 路径 | 4h |

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
| 20 | **完整的类型系统** | 完整的类型检查 + 类型驱动优化 |

---

## 类型系统增强路线图（详细）

### Phase 0: `--strict` 模式（P0#1）

在 `CompilerService::eval()` 中增加最简开关：

```
eval():
  TypeCheckWrap tc_pass
  tc_pass.check_before_lowering(...)
+ if (strict_mode_ && tc_pass.has_type_error()) return error
```

- 新增 `set_strict_mode(bool)` + `strict_mode_` 字段
- `--serve` 新增 `config strict true` 命令
- 改造 `TypeCheckWrap::has_error()` —— 当前永远返回 `false`
- strict 默认为 false，不破坏现有 tests

### Phase 1: 增量类型缓存（P0#2）

现状：`dirty_` 存在，`synthesize_flat` 每次检查 `is_dirty(id)`，但只跳过 synthesis，不缓存结果。

动作：
- 在 `synthesize_flat` 每个分支末尾写 `flat.set_type_id(id, result.index)`
- `infer_flat()` 的 `cs_.normalize()` 后也写入
- 已有的 `mark_subtree_dirty` 自动触发重检查

### Phase 2: 类型信息流入 IR（P1#4）

- `IRInstruction` 增加 `uint32_t type_id = 0`（0=dynamic，向后兼容）
- Lowering 时从 `flat.type_id(id)` 写入 `inst.type_id`
- IRInterpreter 在 strict 模式下做运行时类型断言

### Phase 3: Let-Poly（P1#5）

- `synthesize_flat_let` 对 let 绑定做泛化（自由类型变量 → Forall）
- `synthesize_flat_var` 已有 `instantiate_all` 骨架，替换自由变量
- 只在 strict 模式下启用

### Phase 4: PassManager 集成（P1#6）

- 新建 `TypeSpecializationPass`（参考 `ConstantFoldingWrap` 写法）
- 类型感知优化：已知类型解除 coercion 冗余、死代码消除
- 插入 IR 管线：`lower → [TypeSpecialization] → ComputeKind → Arity → ConstFold → execute`

## 测试状态

```
smoke:      5/5  ✅
integ:     87/87 ✅
unit:      61/61 ✅
bash:     106/106 ✅
AI Agent:   —   ✅ (DeepSeek v4 Flash, EDSL restore 工作正常)
```

## 代码统计（5/18 收盘）

```
src/core/       ~2,700 行
src/parser/     ~1,400 行
src/compiler/   ~10,000 行
lib/std/        12 files ~450 行 Aura
docs/           tutorial.md + known_issues.md + roadmap.md + 设计文档
tests/          3 suites + bash 回归 + AI agent EDSL 管线
```

## 今天的提交（17 commits）

```
fcd95e0  ArenaGroup 集成
e5393e0  模块级增量编译
1e6fd2b  磁盘缓存
4dad634  docs 二次翻新（design 仓库）
2d3abec  类型 coercion + arity 修复
4de8f9b  递归函数 IR 缓存
192c1b7  Pair IR 原生指令
205071f  标准库 P0
dd69965  Variadic lambda 支持
49c3e0c  标准库 variadic 重构
8e313dc  Add 'apply' built-in
00109a6  Fix hash persistence + REPL env tracking
338f93d  string->number trim + display/stdout fix
fbb7a5a  Add char primitives
b2ae103  Add format primitive (SRFI-28)
5ba86d1  Parser error recovery
3cb9a33  Fix recursive function IR caching
```
