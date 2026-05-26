# Aura 路线图

**更新：2026-05-26 — Phase 5 全部完成，待做 4 项**

---

## 项目状态

| 维度 | 数值 |
|:-----|:-----|
| 核心测试 | ✅ 7 suites + REPL 通过 |
| EDSL Benchmark (Grok) | 110/135 (81.5%) |
| EDSL Benchmark (DeepSeek) | 101/135 (74.8%) |
| AOT emit 测试 | ✅ 57/57 全绿 |
| Benchmark 自托管 | ⚠️ pass rate 差距大（~10% vs Python 75-81%） |

---

## Phase 5 — 自托管 Benchmark 性能达标

### 已完成

**P0 — 基础设施**
- [x] `http-post` 加 curl 超时 + pipe+fork execvp
- [x] `http-post` 改用 libcurl C API（dlopen 运行时加载，零子进程）
- [x] `extract-code` 加 `(define ...)` / `(display ...)` fallback
- [x] `LLM_BASE_URL` / `LLM_MODEL` 环境变量支持

**P1 — 智能调度**
- [x] `run-one` 改用 `intend` 控制器（generator/verifier/fixer）
- [x] ant colony 局部变异修复（`colony:search`）
- [x] `check_success` 灵活 substring 匹配
- [x] TASK_HINTS 从 bench-tasks.json 注入 system prompt
- [x] 简洁 system prompt（去掉 76 行 API ref）

**P2 — 并行化**
- [x] 并行执行（`run_parallel.sh`, `BENCH_OFFSET`）
- [x] `fiber:spawn` 原语（serve 模式可接 fiber scheduler）
- [x] `session:create` 原语（隔离 evaluator，独立 string_heap_/pairs_）
- [x] `run_serve.py` serve-async 多 session 编排框架

**P3 — 原生 HTTP + REPL**
- [x] libcurl C API（dlopen，零子进程）
- [x] curl/curl.h 条件编译（CI 兼容）
- [x] REPL 测试修复 + 集成 build.py CI_CORE

**修复的 Bug**
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

### 待做
- [ ] **Serve 模式**：bench.aura 改用 `--serve-async` 持久化 workspace
- [x] **错误诊断传播**：`set-code`/`eval-current-output` 不吞空字符串
- [ ] **JSON 结果输出**：兼容 `bench_results/*.json` 格式
- [ ] **回归监控**：git commit + 模型 + 过率
