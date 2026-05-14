# Aura

**为 AI Agent 设计的编程语言** — C++26 实现，从最小 Lisp 核心自然生长。

---

## 当前状态（2026.05.14）

```
M1 求值器   ✅  树遍历 + FlatAST + AuraIR (21 opcodes) + IR 解释器
M2 查询引擎 ✅  ASTIndex + QueryEngine + TransformEngine + HotSwap
M3a 语言补全 ✅  布尔/序对/begin/set!/quote/cond
M3b 宏系统   ✅  defmacro + 卫生宏 gensym + 编译期验证
M3c 反射     ✅  P2996 schema + IR opcode 验证
M3d 类型系统 ✅  L6.1-L6.7: 渐进类型 + Occurrence Typing + 类型查询
M3e 工具链   ✅  Benchmark + 增量编译 + --serve 协议
M3f AI 闭环   ✅  mutation_loop.py + LLM 驱动变异
M4 生产     ⬜  LLVM + AOT + 自举
```

### 核心管线

```
输入 ──→ Parser (FlatAST) ──→ TypeChecker ──→ Lowering ──→ PassManager ──→ IR Executor
                │                  │               │              │
                ↓                  ↓               ↓              ↓
           Cache (ABF v2)   类型标注 + 约束     IR 缓存       compute-kind
                                               (by-name)      arity-check
                                                              const-fold
```

### 测试覆盖

```
CTest: 49 tests     Benchmark: 42 cases    Integration: 57 tests
Typecheck: 10 cases Smoke: 5 tests         Mutation: 6 modes    Demo: 1
```

```bash
$ python3 build.py test all
All 7 test suites passed
```

---

## 快速开始

```bash
# 构建
cmake -B build && cmake --build build --target aura

# 求值
echo '(+ 1 2)' | ./aura                      # → 3
echo '((lambda (x) (* x 2)) 5)' | ./aura --ir # → 10

# 查询/变换 AST
echo '(+ 1 2)' | ./aura --query '(node-type Call)'           # → 1 matches
echo '(+ 1 2)' | ./aura --query-and-fix '(node-type Call)'    # → applied=true
echo '(+ x 1)' | ./aura --serve                                # → error + auto-fix

# 类型检查 (--typecheck)
echo '42' | ./aura --typecheck               # → type: Int

# 缓存
echo '(+ 1 2)' | ./aura --cache /tmp/x.abc   # → cache written + result

# REPL
./aura                                        # → Aura v0.1
> (+ 1 2)
3
```

---

## 源码结构

```
src/
├── core/           arena, ast, ast_flat (SoA), ast_pool, type, type_info
├── parser/         lexer, flat_parser
├── compiler/
│   ├── evaluator           — 树遍历求值器 (FlatAST 直接求值)
│   ├── ir                  — AuraIR 指令集 (21 opcodes)
│   ├── ir_lowering         — FlatAST → IR (cache-aware)
│   ├── ir_executor         — IR 解释器 (闭包 + cells + coercion)
│   ├── pass_manager        — concept-based Pass pipeline
│   ├── compute_kind        — Known/Unknown 数据流分析
│   ├── arity               — 参数数量校验
│   ├── cache               — ABF v2 列式缓存 (write + mmap read)
│   ├── diag                — 结构化错误诊断
│   ├── service             — CompilerService (持久化会话)
│   ├── query               — ASTIndex / QueryEngine / TransformEngine
│   ├── type_checker        — 渐进类型 + Occurrence Typing
│   └── types               — EvalValue 类型格式化
├── tools/
│   ├── aura-reflect        — P2996 IR opcode/schema 验证
│   └── aura-schema         — JSON Schema 生成
└── main.cpp                — CLI 入口
tests/
├── test_ir.cpp             — 管线/查询/内存池集成测试
├── benchmark.py            — 42 个回归基准
├── mutation_loop.py        — AI 驱动变异循环
├── agent_demo.py           — 7 种 Agent 模式演示
└── build.py                — 统一构建/测试入口
```

---

## 关键设计文档

| 文档 | 内容 |
|------|------|
| [`docs/aura_typesystem.md`](docs/aura_typesystem.md) | 类型系统设计（代码验证标注版） |
| [`docs/hygienic_macros.md`](docs/hygienic_macros.md) | 卫生宏设计 |
| [`docs/module_system_abf_v2.md`](docs/module_system_abf_v2.md) | 模块系统 + ABF v2 序列化 |
| [`docs/incremental_caas.md`](docs/incremental_caas.md) | 增量编译 + Compiler as a Service |
| [`docs/ast_to_source.md`](docs/ast_to_source.md) | FlatAST → S-表达式反解 |
| [`docs/roadmap.md`](docs/roadmap.md) | 完整路线图 |

---

## 设计哲学

1. **最小核心** — 只保留最少的语义原语，上层语法都是宏。
2. **代码即数据** — Homoiconic，AI 直接操作 AST。
3. **自然生长** — 从真实语义需求中慢慢长大，不拔苗助长。
4. **AI 优先** — 默认动态类型 + 渐进加固；结构化 blame 便于自动修复。

---

## 许可证

Apache 2.0
