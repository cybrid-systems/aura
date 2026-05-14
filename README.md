# Aura

**为 AI Agent 设计的编程语言** — C++26 实现，从最小 Lisp 核心自然生长。

---

## 当前状态（2026.05.14）

```
M1 求值器   ✅  纯 FlatAST 管线 (SoA)，Expr* 指针树全部移除
M2 查询引擎 ✅  ASTIndex + QueryEngine + TransformEngine + HotSwap + Serve
M3a 语言补全 ✅  布尔/序对/begin/set!/quote/cond/macro/letrec/string/vector
M3b 宏系统   ✅  defmacro + SyntaxMarker 列 (卫生宏 Phase1)
M3c 反射     ✅  P2996 schema + IR opcode + 结构布局验证
M3d 类型系统 ✅  L6.1-L6.7: 渐进类型 + Occurrence Typing + 类型查询
M3e 工具链   ✅  Benchmark(44) + 增量编译 + --serve + CI (GitHub Actions)
M3f AI 闭环   ✅  mutation_loop.py + ai_agent_demo.py (6 scenes)
M4a 缓存     ✅  ABF v2 列式缓存 (v4, O(1) resolve, SyntaxMarker)
M4b AI 协议   ✅  docs/ai_agent_protocol.md
```

### 核心管线

```
输入 ──→ Parser (FlatAST) ──→ TypeChecker ──→ Lowering ──→ PassManager ──→ IR Executor
                │                  │               │              │
                ↓                  ↓               ↓              ↓
           Cache (ABF v4)    类型标注 + 约束     IR 缓存       compute-kind
           SyntaxMarker                                    arity-check / const-fold
```

### 测试覆盖

```
CTest: 49       Benchmark: 44       Integration: 62
Typecheck: 10   Smoke: 5             Mutation: 6 modes
Agent Demo: 1   AI Agent Demo: 6
```

```bash
$ python3 build.py test all
All 8 test suites passed
```

---

## 快速开始

```bash
# 构建
cmake -B build && cmake --build build --target aura

# 求值
echo '(+ 1 2)' | ./aura                      # → 3
echo '((lambda (x) (* x 2)) 5)' | ./aura --ir # → 10

# 类型检查 + blame
echo '42' | ./aura --typecheck               # → type: Int
echo '(+ "x" 1)' | ./aura --typecheck        # → coercion error + blame

# 向量
echo '(vector-ref (vector 10 20 30) 1)' | ./aura   # → 20

# AST 查询/变换
echo '(+ 1 2)' | ./aura --query '(node-type Call)'  # → 1 matches
echo '(+ x 1)' | ./aura --serve                      # → error + auto-fix

# 缓存 (v4 O(1) resolve)
echo '(+ 1 2)' | ./aura --cache /tmp/x.abc
./aura --cache-open /tmp/x.abc

# 递归 + 闭包
echo '(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 10))' | ./aura

# AI Agent 演示
python3 tests/ai_agent_demo.py
```

---

## 源码结构

```
src/
├── core/           arena, ast (FlatAST+StringPool), type
├── parser/         lexer, parser (S-表达式 → FlatAST)
├── compiler/
│   ├── evaluator          — 树遍历求值器 (纯 FlatAST)
│   ├── ir / lowering      — AuraIR 21 opcodes + IR 降低
│   ├── ir_executor        — IR 解释器
│   ├── pass_manager       — Pass pipeline (compute-kind/arity/const-fold)
│   ├── cache              — ABF v4 列式缓存 (O(1) resolve)
│   ├── query              — ASTIndex + QueryEngine + TransformEngine
│   ├── type_checker       — L6.1-L6.7 渐进类型
│   ├── service            — CompilerService (增量/serve/hot-swap)
│   ├── diag / value       — 诊断 + EvalValue 运行时值
│   └── reflect.ixx        — P2996 auto_to_json
├── tools/           aura-reflect, aura-schema
└── main.cpp         CLI
tests/
├── test_ir.cpp             — 49 CTest (管线/查询/内存池)
├── benchmark.py            — 44 回归基准 + --check/--update
├── mutation_loop.py        — AI 驱动变异循环
├── ai_agent_demo.py         — 6 场景工具链演示
└── build.py                — 8 套件统一入口
.github/workflows/ci.yml    — GitHub Actions CI
```

---

## 关键设计文档

| 文档 | 内容 |
|------|------|
| [`docs/aura_typesystem.md`](docs/aura_typesystem.md) | 类型系统设计（代码验证标注版） |
| [`docs/ai_agent_protocol.md`](docs/ai_agent_protocol.md) | AI Agent 工具定义 + Zero-Shot 工作流 |
| [`docs/hygienic_macros.md`](docs/hygienic_macros.md) | 卫生宏 + SyntaxMarker 设计 |
| [`docs/module_system_abf_v2.md`](docs/module_system_abf_v2.md) | 模块系统 + ABF v2 序列化 |
| [`docs/incremental_caas.md`](docs/incremental_caas.md) | 增量编译 + Compiler as a Service |
| [`docs/ast_to_source.md`](docs/ast_to_source.md) | FlatAST → S-表达式反解 |
| [`docs/roadmap.md`](docs/roadmap.md) | 完整路线图 |
| [`docs/ai_agent_protocol.md`](docs/ai_agent_protocol.md) | AI Agent 工具协议 |

---

## 设计哲学

1. **最小核心** — 只保留最少的语义原语，上层语法都是宏。
2. **代码即数据** — Homoiconic，AI 直接操作 AST。
3. **自然生长** — 从真实语义需求中慢慢长大，不拔苗助长。
4. **AI 优先** — 默认动态类型 + 渐进加固；工具优先于生成。

---

## 许可证

Apache 2.0
