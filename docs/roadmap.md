# Aura — 路线图

## 当前状态（2026-05-17）

**核心完成度：可写 500+ 行业务代码** ✅

### 已实现

```
✓ 语言核心：100+ 原语、TCO、lambda、let/let*/letrec、cond、match、when/unless
✓ 宏系统：quasiquote、gensym、递归展开、dotted rest param
✓ 模块命名空间：require/import prefix、export、循环检测、自动 lib 发现
✓ 渐进类型：L6 推断、forall、Float、增量 typecheck（dirty skip）
✓ 错误处理：try/catch/raise/assert、原语返回 error
✓ 测试框架：check/check=、test-suite、run-tests
✓ 正则：regex-match? regex-find regex-replace regex-split
✓ 数学：sin cos tan asin acos atan log log10 exp pow sqrt floor ceil round
✓ 文件 IO：read-file write-file file-copy file-delete file-size file-exists? directory-list
✓ EDSL：set-code query:* mutate:* typecheck-current eval-current 15+ 原语
✓ 格式化：--fmt / --fmt -i / --fmt --check
✓ AI Agent：DeepSeek/MiniMax/GPT 通用 + Phase 2 EDSL 工作流 + 自动测试
✓ 标准库：list math string json struct validate test（8 lib）
✓ dotted pair 语法修复
✓ letrec 互递归修复
✓ parse_val quote 支持
✓ pipe mode 只打印最后结果 + 跳过 void
```

### 测试

```
Bash 回归：    106 tests  ✅
C++ test_ir：  61 cases    ✅
集成管线：     50+ cases   ✅
```

### 代码统计

```
src/core/       ~2,500 行  — FlatAST、arena、type
src/parser/     ~1,200 行  — lexer + parser
src/compiler/   ~9,000 行  — evaluator + IR + type_checker + query + EDSL
lib/std/        8 files    — ~300 行 Aura 代码
tests/          C++ 61 + bash 106 + AI Agent
```

## 下一步

### P0 — 语言打磨

| 项 | 说明 | 状态 |
|----|------|------|
| EDSL 自动修复 | 运行时错误 → set-code → query → mutate → eval-current | ✅ 完成 |
| Agent history truncation | 只保留最近 3 轮避免窗口爆炸 | ✅ 完成 |
| Agent 截断检测 | `finish_reason == "length"` 检测 + 警告 | ✅ 完成 |
| `string->number` 非数字返回 #f | 符合 Scheme 语义 | ✅ 完成 |
| `when`/`unless` 特殊形式 | Scheme 兼容 | ✅ 完成 |

### P1 — 功能扩展

| 项 | 说明 | 状态 |
|----|------|------|
| IO 库 | file-copy, delete, size, directory-list | ✅ 完成 |
| Regex 库 | match? find replace split | ✅ 完成 |
| Math 库 | sin cos tan log exp pow sqrt floor ceil round | ✅ 完成 |
| Agent 代码 diff | 仅发送代码变更而非全文重写 | ⬜ |
| 标准库扩充 | 按需 | 🟡 |

### P2 — 性能 & 基础设施

| 项 | 说明 |
|----|------|
| IR 管线默认 | 从树遍历器切换到 IR executor |
| dirty skip 深度优化 | per-module skip |
| LLVM JIT | `--jit` 模式编译到原生代码 |
| AOT 编译 | 静态二进制 |
| 自举 | 用 Aura 写 Aura 编译器 |
