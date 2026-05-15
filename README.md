# Aura

**为 AI Agent 设计的编程语言** — C++26 实现，从最小 Lisp 核心自然生长。

---

## 当前状态（2026.05.15）

```
M1 求值器     ✅  纯 FlatAST 管线 (SoA)，Expr* 指针树全部移除
M2 查询引擎   ✅  ASTIndex + QueryEngine + TransformEngine
M3a 语言补全  ✅  布尔/序对/begin/set!/quote/cond/letrec/string/vector/hash
M3b 宏系统    ✅  defmacro + 卫生宏 Phase 1-5 (SyntaxMarker + 展开器 + 克隆 + 类型检查)
M3c 反射      ✅  P2996 schema + kNodeMeta + IR opcode + 结构布局验证
M3d 类型系统  ✅  L6.1-L6.8: 渐进类型 + Occurrence + forall 多态 + Float
M3e 工具链    ✅  Benchmark(44) + 增量编译 + --serve + CI
M3f AI 闭环    ✅  mutation_loop + LLM 驱动 + AI Agent 演示(6 场景)
M4a 缓存      ✅  ABF v2 列式缓存 (v4, O(1) resolve, SyntaxMarker)
M4b AI 协议    ✅  docs/ai_agent_protocol.md (7 工具定义)
M4c 模块系统  ✅  import + AURA_PATH + ABF v2 全链路
M4d 自进化     🚧  docs/typed_mutation_design.md (设计阶段)
```

### 核心管线

```
输入 ──→ Parser (FlatAST) ──→ TypeChecker ──→ Lowering ──→ PassManager ──→ IR Executor
                │                  │               │              │
                ↓                  ↓               ↓              ↓
           Cache (ABF v4)    forall 实例化      IR 缓存       compute-kind
           SyntaxMarker      Occurrence         ConstF64      arity / const-fold
           AURA_PATH         Float type         (float ops)
```

### 测试覆盖

```
CTest: 52/52   Benchmark: 44/44   Integration: 62/62
```

```bash
$ ctest
100% tests passed, 0 tests failed out of 52
```

---

## 快速开始

```bash
# 构建
cmake -B build && cmake --build build --target aura

# 求值
echo '(+ 1 2)' | ./aura                      # → 3
echo '((lambda (x) (* x 2)) 5)' | ./aura --ir # → 10

# 浮点数
echo '(+ 3.14 1.5)' | ./aura                 # → 4.640000000000001
echo '(/ 10.0 3.0)' | ./aura --ir            # → 3.3333333333333335

# 类型检查 + forall 多态
echo '42' | ./aura --typecheck                # → type: Int
echo '(map (lambda (x) (+ x 1)) (list 1 2 3))' | ./aura --typecheck  # → type: Int

# 跨行 define + 闭包
cat <<EOF | ./aura
(define fib (lambda (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))))
(fib 10)
EOF
# → 55

# 模块导入 (AURA_PATH)
export AURA_PATH="/usr/local/lib/aura"
echo '(import "math") (add 2 3)' | ./aura

# 哈希表
echo '(hash-set! h "key" 42) (hash-ref h "key")' | ./aura

# 缓存序列化 (ABF v4)
echo '(+ 1 2)' | ./aura --cache /tmp/x.abc
./aura --cache-open /tmp/x.abc

# AI Agent 演示
python3 tests/ai_agent_demo.py
```

---

## 源码结构

```
src/
├── core/           arena, ast (FlatAST+StringPool), type (TypeRegistry+TypeTag)
├── parser/         lexer, parser (S-表达式 → FlatAST, Float 字面量)
├── compiler/
│   ├── evaluator          — 树遍历求值器 (纯 FlatAST, 含 float 支持)
│   ├── ir / lowering      — AuraIR 23 opcodes + IR 降低 (含 ConstF64)
│   ├── ir_executor        — IR 解释器 (int/float 混合运算)
│   ├── pass_manager       — Pass pipeline (compute-kind/arity/const-fold)
│   ├── cache              — ABF v4 列式缓存 (O(1) resolve)
│   ├── query              — ASTIndex + QueryEngine + TransformEngine
│   ├── type_checker       — L6.1-L6.8: 渐进类型 + forall + Float + Occurrence
│   ├── service            — CompilerService (增量/serve/module/hot-swap)
│   ├── diag / value       — 诊断 + EvalValue 运行时值 (containing double)
│   └── reflect.ixx        — P2996 auto_to_json
├── tools/           aura-reflect, aura-schema
└── main.cpp         CLI (含 --serve, --cache-open, --typecheck)
tests/
├── test_ir.cpp             — 52 CTest (管线/查询/类型/forall/内存池)
├── benchmark.py            — 44 回归基准
├── mutation_loop.py        — AI 驱动变异循环
├── ai_agent_demo.py         — 6 场景工具链演示
├── build.py                — 8 套件统一入口
└── fixtures/               — 测试模块 (.aura 文件)
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
| [`docs/typed_mutation_design.md`](docs/typed_mutation_design.md) | 🆕 类型安全变异算子设计 |
| [`docs/roadmap.md`](docs/roadmap.md) | 完整路线图 |

---

## 设计哲学

1. **最小核心** — 只保留最少的语义原语，上层语法都是宏。
2. **代码即数据** — Homoiconic，AI 直接操作 AST。
3. **自然生长** — 从真实语义需求中慢慢长大，不拔苗助长。
4. **AI 优先** — 默认动态类型 + 渐进加固；工具优先于生成。
5. **自进化** — 变异操作有类型保证，进化可审计可回滚。

---

## 许可证

Apache 2.0
