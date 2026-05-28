# Aura 路线图

**更新：2026-05-28 — Phase 1-7 全部完成，Aura-native benchmark Grok 100%**

---

## 项目状态

| 维度 | 数值 |
|:-----|:-----|
| 核心测试 | ✅ 8 suites + REPL 通过 |
| EDSL Benchmark (Grok) | **135/135 (100%)** — 首次生成 22%, ant colony 78% |
| EDSL Benchmark (DeepSeek) | 106/135 (78.5%) — Aura-native, 10 workers |
| AOT emit 测试 | ✅ 57/57 全绿 |
| Aura-native Benchmark | ✅ **100%**（Grok, 6 workers, ~2min） |
| 总 commits | 1100+ |

---

## 已完成（Phase 1-7）

### Phase 1-4 — EDSL 基础能力
- `query:*` / `mutate:*` / `ast:*` — AST 查询、手术编辑、快照/diff
- `workspace:*` — 分层 workspace（create/switch/merge/lock/discard）
- `synthesize:*` — 代码模板 / LLM 生成 / 遗传优化 / 管线
- `fiber:spawn` / `session:create` — 异步 + 隔离 evaluator
- `rule:*` — 代码规范系统（pattern/replace/condition）
- 类型系统 — Sound Gradual Typing 全链路

### Phase 5 — 自托管 benchmark 改造
- `http-post` libcurl C API（dlopen，零子进程）
- `intend` 控制器（C++ 原语，gen→ver→fix→retry）
- `check_success` C++ 原语 + 字边界 guard + 结构化错误检测
- `std/extract.aura` — 6 步 code extraction
- `std/prompt.aura` — 统一 prompt builder
- `run_parallel.sh` — 多进程并行执行
- Ant colony 局部变异修复（零 LLM 开销）

### Phase 6 — EDSL 深度强化 + 自托管闭环
- **P0:** Aura-native benchmark 100%（Grok, 6 workers, ~2min）
- **P1:** workspace 隔离修复 + 源码级 merge
- **P2:** 多 worker benchmark runner（`run_parallel.sh` + BENCH_OFFSET/LIMIT）
- **P3:** `std/heal.aura` — AST 手术级 diagnose + apply-fix（`query:node-type` + `mutate:wrap`）
- **P4:** `std/query.aura` — 组合查询（`query:filter` / `query:uncalled` / `query:callers-of`）
- **P5:** 双 Arena 内存管理 — `persistent_arena_` + `temp_arena_` + `gc-temp`

### Phase 7 — Agent 编排与高层抽象
- `std/orchestrator.aura` — 编排框架（define-role / step / pipeline / run / parallel）
- `std/refactor.aura` — 重构操作（rename-var / extract-function / inline-function）
- 全部纯 Aura，复用现有 `query` / `mutate` / `intend` / `heal` 原语

---

## 关键决策记录

### 双 Arena 内存管理
- `persistent_arena_` — 模块 AST、while 循环闭包
- `temp_arena_` — task 闭包体、临时 env
- `gc-temp` 不清 `pairs_`/`string_heap_`（结果列表引用这些 vector）
- `cells_` 完全保护（与 env 绑定共享，清掉 = 所有函数绑定悬空）

### Benchmark 运行方式
- **Aura-native（推荐）：** `bash tests/run_parallel.sh N`
- **Python runner：** `python3 tests/edsl_benchmark.py`
- 两路通过率已对齐（DeepSeek 76-79%），Grok 上 Aura 独占 100%

### orchestrator 不接入 benchmark
benchmark 是固定管线，直接 C++ `intend` 是最优路径。编排框架为动态场景设计（LLM 自描述管线）。

---

## 已知问题
- `string-index` 非内置（仅在 `std/adaptive`）
- 字符串位置迭代产生 `<kwd>` 垃圾值（绕行：`string->list` + list ops）
- `(require "a" "b" all:)` 语义可能只对最后模块应用 `all:`（绕行：分两个 require）
- `--serve-async` fiber scheduler 的 epoll 交互问题（单进程 fiber 并行待修复）
