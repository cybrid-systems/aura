# Aura — 路线图

---

## 能力矩阵

| 领域 | 状态 | 备注 |
|------|------|------|
| **语言核心** | ✅ | ~80 原语、TCO、lambda、let/let\*/letrec、match、define-struct |
| **宏系统** | ✅ | quasiquote、gensym、递归展开、dotted rest param |
| **渐进类型** | ✅ | L6 推断、forall、Float、增量 typecheck（dirty 跟踪 + 跳过 clean） |
| **模块命名空间** | ✅ | `use`/`module-get`/`import prefix`/`export`/循环检测/自动 lib 发现 |
| **错误处理** | ✅ | `try`/`catch`/`raise` + error variant + 原语不崩溃 |
| **测试框架** | ✅ | `check`/`check=` + `test-suite` 宏 + bash runner |
| **标准库** | ✅ | list / math / string / json / struct / validate / test（~270 行 Aura） |
| **格式化** | ✅ | `--fmt` / `--fmt -i` / `--fmt --check` |
| **EDSL** | ✅ | 15+ query/mutate 原语 + workspace 模型 + 增量类型检查 |
| **AI Agent** | ✅ | DeepSeek/MiniMax/GPT 通用接口 + 优化 prompt |
| **IR 管线** | 🟡 | IR executor 可用但非默认路径（99% 走树遍历器） |
| **`--serve`** | ✅ | JSON Lines 协议 + 所有 EDSL 支持 |

## 测试状态

```
Unit (C++ test_ir):     61 cases  ✅
Bash 回归:              77 tests  ✅
集成 (eval/ir/tc/serve): 50+ cases ✅
```

## 代码统计

```
src/core/                  ~2,500 行  — FlatAST、arena、type
src/parser/                ~1,200 行  — lexer + parser
src/compiler/              ~8,800 行  — evaluator + IR + type_checker + query + EDSL
lib/std/                   7 files    — ~270 行 Aura 代码
tests/                     C++ 22 + bash 77 + AI Agent
```

## 下一步

### P0 — 让语言能支持任意 500+ 行项目

| 项 | 工作量 | 影响 | 说明 |
|----|--------|------|------|
| **AI Agent 闭环打磨** | 1-2 天 | 🔴 | 当前 prompt 已大幅改进仍需迭代：LLM 有时仍幻觉。改进方向：(1) 多文件项目支持（agent 一次写一个模块）(2) Phase2 EDSL 修复工作流 (3) `run-tests` 原语完成 |
| **标准库扩充** | 2-3 天 | 🔴 | 当前 7 个库够用但不够全。缺 IO (`read-file`/`write-file`)、文件系统、regex。这些是真实项目的必需品 |
| **多文件项目支持** | 1 天 | 🟡 | AI Agent 一次可以生成多个 `.aura` 文件。需完善 `import` 路径解析和工作流 |

### P1 — 让 1000+ 行项目可维护

| 项 | 说明 |
|----|------|
| **dirty 优化** | 当前 per-node dirty，可加 per-module skip 和大块 skip |
| **IR 管线默认** | IR executor 已存在但 99% 走树遍历器。切换可显著加速执行 |
| **export 检查** | 循环依赖检测代码已就绪，等待多模块场景 |

### P2 — 修复 & 质量

| 项 | 说明 |
|----|------|
| **Dotted pair 语法** | ✅ 已修复（缺完整的有序显示） |
| **letrec 互递归** | ✅ 已修复（多 binding 展开为 define + set!） |
| **所有测试通过** | ✅ 77/77 |
| **多 body 表达式** | ✅ 已修复（let、lambda 等特殊形式） |
| **parse_val 支持 quote** | ✅ 已修复 |
| **管道模式** | ✅ 只打印最后结果、跳过 void、2>&1 捕获 display |

### 极远期

```
LLVM JIT 降级    — --jit 模式编译到原生代码
AOT 编译         将 Aura 代码编译为静态二进制
自举             用 Aura 写 Aura 编译器
```
