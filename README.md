# Aura

**为 AI Agent 设计的编程语言** — C++26 实现，从最小 Lisp 核心自然生长。

---

## 当前状态（2026.05.16）

```
M1-M3  语言核心          ✅  FlatAST管线/宏系统/反射/类型系统/工具链
M4     高级特性          ✅  AI闭环/缓存/模块系统/Typed Mutation
M5     语言完善          ✅  变参算术/TCO/equal?/match/define-struct/cXr简写
M6     标准库            ✅  list/math/string/json (AURA_PATH 可导入)
P7     宏系统 v2         🟡  设计就绪 (quasiquote/gensym/递归展开)
P8     生产后端          ⬜  LLVM JIT / AOT / 自举
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
CTest: 52/52   Benchmark: 44/44   Integration: 62/62   std lib: 168 行 Aura 代码
```

```bash
$ ctest
100% tests passed, 0 tests failed out of 52

$ AURA_PATH=./lib echo '(import "std/list") (sort (list 3 1 4 1 5))' | ./aura
# → (1 1 3 4 5)
```

---

## 快速开始

```bash
# 构建
cmake -B build && cmake --build build --target aura

# 求值
echo '(+ 1 2  󰊯 3 4 5)' | ./aura            # → 15  (变参算术)
echo '(+ 3.14 1.5)' | ./aura                 # → 4.64 (float 自动提升)
echo '(filter (lambda (x) (> x 5)) (list 3 7 2 9))' | ./aura  # → (7 9)
echo '(foldl + 0 (list 1 2 3 4 5))' | ./aura   # → 15

# 类型检查 + 多态
echo '(map (lambda (x) (* x 2)) (list 1 2 3))' | ./aura --typecheck

# 递归 + TCO (20000 层不爆栈)
cat <<EOF | ./aura
(define (deep n) (if (< n 0) 0 (deep (- n 1))))
(deep 20000)
EOF

# 模式匹配
cat <<EOF | ./aura
(match (list 1 2 3)
  ((list a b c) (+ a b c))
  (_ 0))
EOF

# 命名结构体
cat <<EOF | ./aura
(define-struct point (x y))
(define p (make-point 10 20))
(point-x p)  ; → 10
EOF

# 标准库 (AURA_PATH)
export AURA_PATH="./lib"
echo '(import "std/list") (sort (list 9 2 7 4 1 8 5 3 6))' | ./aura
echo '(import "std/json") (json-parse "{\"a\":1}")' | ./aura

# 哈希表 + 文件 I/O
echo '(hash-set! h "key" 42) (hash-ref h "key")' | ./aura
echo '(read-file "data.txt")' | ./aura

# --serve (AI Agent 模式)
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
| 文档 | 状态 | 内容 |
|------|------|------|
| [`docs/aura_typesystem.md`](docs/aura_typesystem.md) | ✅ 实现完成 | 渐进类型 + Occurrence + forall + Float |
| [`docs/macro_system_v2.md`](docs/macro_system_v2.md) | 🟡 设计就绪 | quasiquote + gensym + 递归宏展开 |
| [`docs/ai_agent_protocol.md`](docs/ai_agent_protocol.md) | ✅ 实现完成 | AI Agent 工具定义 + Zero-Shot 工作流 |
| [`docs/hygienic_macros.md`](docs/hygienic_macros.md) | ⚠️ 部分过时 | defmacro + SyntaxMarker（v2 设计中） |
| [`docs/module_system_abf_v2.md`](docs/module_system_abf_v2.md) | ✅ 实现完成 | import + AURA_PATH + ABF 序列化 |
| [`docs/typed_mutation_design.md`](docs/typed_mutation_design.md) | ✅ 实现完成 | MutationLog + 类型安全变异算子 |
| [`docs/incremental_caas.md`](docs/incremental_caas.md) | ✅ 实现完成 | Compiler Service + 增量编译 |
| [`docs/ast_to_source.md`](docs/ast_to_source.md) | 🟡 基础实现 | FlatAST → S-表达式反解 |
| [`docs/roadmap.md`](docs/roadmap.md) | ✅ 活跃 | 完整路线图 + 遗留问题 |

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
