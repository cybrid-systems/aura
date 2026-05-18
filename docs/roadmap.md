# Aura — 路线图

**更新：2026-05-18** — 经过两轮 sprint（CaaS + 增量编译 + 语言打磨）后重新评估。

---

## 当前状态评估

### 完成度评分

| 维度 | 分数 | 说明 |
|------|------|------|
| 语言核心求值 | 🟢 7/10 | tree-walker + IR 双路径基本稳定，仍有少数 fallback 路径 |
| 编译器基础设施 | 🟢 7/10 | ArenaGroup / 增量 / 缓存 / 热替换都已就位 |
| 标准库覆盖 | 🟡 4/10 | 10 个文件 ~400 行，缺失 char/string 操作、format |
| 测试覆盖 | 🟡 3/10 | 只有 67 个集成测试，大量新功能无测试覆盖 |
| 错误处理 | 🔴 2/10 | 没有 proper diagnostics，parser 撞第一个错误就停 |
| 类型系统 | 🟡 4/10 | 有 inference 但无 full type check |
| 文档 | 🔴 1/10 | README 之外几乎没有文档 |
| AI agent 集成 | 🟡 5/10 | 框架完整但未用真实 API 测过 |

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

| # | 项 | 说明 | 工作量 | 依赖 |
|---|-----|------|--------|------|
| 1 | 🧪 **hash/variadic/apply 集成测试** | 新功能一个 test case 都没有 | 1h | — |
| 2 | 📖 **快速入门文档** | 写 2-3 页语言教程 | 2h | — |
| 3 | 🔤 **char 标准原语补齐** | `char-alphabetic?`, `char-numeric?`, `char-whitespace?`, `char-upcase`, `char-downcase` | 1h | #char 已实现 |
| 4 | 🧶 **string 操作补齐** | `string-copy`, `string-fill!`, `string->list`, `list->string`, `string-join` | 1h | — |
| 5 | 🎯 **format 原语** | `(format "~a = ~a" x y)` 替代字符串拼接 | 1.5h | — |
| 6 | 🌐 **AI agent 管线实测** | 用真实 DeepSeek/OpenAI API 跑一遍 | 需 API key | — |

### P1 — 短期（提升可靠性和体验，支持 1000 行项目）

| # | 项 | 说明 | 工作量 |
|---|-----|------|--------|
| 7 | **Parser 错误恢复** | 当前遇第一个 parse error 就退出 | 3h |
| 8 | **try/catch IR 指令** | 消除一个主要 fallback 路径 | 4h |
| 9 | **proper Diagnostics** | 集中化错误信息，行号/列号/原因/建议 | 2h |
| 10 | **Benchmark 基线** | 对比 IR vs tree-walker 性能 | 2h |
| 11 | **标准库 v2** | 增加到 15-20 个文件，覆盖常见需求 | 8h |
| 12 | **Module re-export** | `(export ...)` 支持多模块导出链 | 2h |

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
| 20 | **完整的类型系统** | Hinze 风格的 complete type checker |

---

## 测试状态

```
smoke:      5/5  ✅
integ:     87/87  ✅
unit:      15/19  ⚠️ (4 个比较器返回值偏离——#t/#f vs 0/1，远期对齐)
bash:     106/106 ✅
AI Agent:  —      ⬜ 需 API key
```

## 代码统计（5/18 收盘）

```
src/core/       ~2,500 行
src/parser/     ~1,200 行
src/compiler/   ~9,500 行
lib/std/        10 files ~400 行 Aura
tests/          3 suites + bash 回归
```

## 今天的提交（9 commits）

```
8e313dc  Add 'apply' built-in: (apply fn list)
49c3e0c  stdlib: rewrite with variadic lambda support
dd69965  Variadic lambda support: (lambda (x . rest) ...) parser + tree-walker
00109a6  Fix hash persistence + REPL value define env tracking
186210f  IR opcode metadata table + consolidate tree-walker fallback names
027421c  IR executor: kOpcodeInfo validation + deduplicate coercion
5cb7689  ir: add kPrimNames table, replace prim_names[] in executor
338f93d  string->number: trim whitespace; display/newline: fix stdout output
fbb7a5a  Add char primitives: char=?, char<?, char->integer, integer->char
```
