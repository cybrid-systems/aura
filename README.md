# Aura

**为 AI Agent 设计的编程语言** — C++26 实现，从最小 Lisp 核心自然生长。

## 这是什么

Aura 是一个 Lisp 方言（类似 Scheme），核心差异点：

- **`--serve` 协议** — 通过 JSON Lines 与 AI Agent 交互，定义、执行、读结果、修复错误，完整的闭环
- **基元可传值** — `+` 也是可传递的函数：`(foldl + 0 (list 1 2 3 4 5))` → 15
- **宏系统 v2** — quasiquote + gensym，`define-struct` 用宏实现
- **渐进类型系统** — 从动态开始，逐步加固
- **Typed Mutation** — AI 驱动的 AST 变异，类型安全可审计

---

## 快速入门

### 构建

```bash
cmake -B build && cmake --build build --target aura
```

### Hello World

```bash
echo '"Hello, World!"' | ./build/aura
# → "Hello, World!"
```

### 加减乘除

```bash
# 变参算术：任意多个参数
echo '(+ 1 2 3 4 5)' | ./build/aura          # → 15
echo '(* 2 3 4)' | ./build/aura              # → 24

# float 自动提升：一个 float 参数就够了
echo '(+ 1 2.5 3)' | ./build/aura             # → 6.5
```

### 定义函数

```bash
echo '(define (square x) (* x x)) (square 12)' | ./build/aura
# → 144
```

### 递归 + TCO

```bash
cat <<EOF | ./build/aura
(define (fact n)
  (if (< n 2) 1
    (* n (fact (- n 1)))))
(fact 10)
EOF
# → 3628800
```

### 列表操作

```bash
echo '(map (lambda (x) (* x 2)) (list 1 2 3))' | ./build/aura
# → (2, 4, 6)

echo '(filter (lambda (x) (> x 5)) (list 3 7 2 9))' | ./build/aura
# → (7, 9)

echo '(foldl + 0 (range 1 101))' | ./build/aura
# → 5050
```

### 标准库

```bash
export AURA_PATH="./lib"

# 排序
echo '(import "std/list") (sort (list 3 1 4 1 5 9))' | ./build/aura
# → (1, 1, 3, 4, 5, 9)

# 数学
echo '(import "std/math") (factorial 10)' | ./build/aura
# → 3628800

# JSON
echo '(import "std/json") (json-stringify (list 1 2 3))' | ./build/aura
# → "[1,2,3]"

# 字符串
echo '(import "std/string") (string-split "a,b,c" ",")' | ./build/aura
# → ("a", "b", "c")

# 结构体
cat <<EOF | ./build/aura
(import "std/struct")
(define-struct point (x y))
(point-x (make-point 10 20))
EOF
# → 10
```

### 模式匹配

```bash
cat <<EOF | ./build/aura
(match (list 1 2 3)
  ((list a b c) (+ a b c))
  (_ 0))
EOF
# → 6
```

### `--serve` AI Agent 模式

```bash
# 启动服务
./build/aura --serve

# 在另一个终端发送命令
echo '{"cmd":"exec","code":"(+ 1 2)"}' | nc localhost
# → {"status":"ok","value":"3"}
```

或使用 Python demo：

```bash
python3 tests/ai_agent_serve.py       # 协议能力演示
export OPENAI_API_KEY="sk-..."
python3 tests/ai_agent_llm.py "斐波那契数列第20项"
# → AI 生成 Aura 代码 → 执行 → 返回 6765
```

---

## 谁该用 Aura

| 场景 | 合适吗？ |
|------|---------|
| AI Agent 驱动的代码生成 | ✅ 这就是设计目标 |
| 嵌入式 DSL | ✅ 宏系统 + homoiconic |
| 学习 Lisp/函数式编程 | ✅ 最小核心，容易上手 |
| 生产级应用开发 | ⚠️ 还在成长期，LLVM JIT 待做 |
| 高性能计算 | ❌ 没有 JIT，速度不是目标（现在） |

---

## 当前状态

```
M1-M3  语言核心          ✅  FlatAST管线/宏系统/反射/类型系统/工具链
M4     高级特性          ✅  AI闭环/缓存/模块系统/Typed Mutation
M5     语言完善          ✅  变参算术/TCO/equal?/match/define-struct/cXr简写
M6     标准库            ✅  list/math/string/json/struct/validate
P7     宏系统 v2         ✅  quasiquote/gensym/递归展开/define-struct 宏
P8     生产后端          ⬜  LLVM JIT / AOT / 自举
```

### 测试

```
CTest: 52/52   Benchmark: 44/44   AI Agent 演示: 30/30
标准库: ~270 行 Aura 代码 (6 个文件)
基元: ~70   源文件: 33   C++ 代码: ~11000 行
```

---

## 源码结构

```
src/
├── core/           arena, ast (FlatAST+StringPool), type
├── parser/         lexer, parser (S-表达式 → FlatAST)
├── compiler/
│   ├── evaluator          — 树遍历求值器
│   ├── ir / lowering      — AuraIR 23 opcodes
│   ├── ir_executor        — IR 解释器
│   ├── pass_manager       — Pass pipeline
│   ├── cache              — ABF v4 列式缓存
│   ├── query              — ASTIndex + QueryEngine
│   ├── type_checker       — 渐进类型 + forall + Occurrence
│   ├── service            — CompilerService (增量/--serve)
│   ├── value              — EvalValue 运行时值 (10 种变体)
│   └── diag               — 诊断系统
├── tools/           aura-reflect, aura-schema
└── main.cpp         CLI (--serve/--typecheck/pipe mode)
lib/std/             标准库 (.aura 文件)
tests/               CTest + Python 测试套件
docs/                设计文档
```

---

## 关键设计文档

| 文档 | 状态 |
|------|------|
| [`docs/aura_typesystem.md`](docs/aura_typesystem.md) | ✅ 渐进类型 + forall + Float |
| [`docs/macro_system_v2.md`](docs/macro_system_v2.md) | ✅ quasiquote + gensym |
| [`docs/ai_agent_protocol.md`](docs/ai_agent_protocol.md) | ✅ 7 工具定义 + Zero-Shot 工作流 |
| [`docs/typed_mutation_design.md`](docs/typed_mutation_design.md) | ✅ Typed Mutation 三阶段 |
| [`docs/module_system_abf_v2.md`](docs/module_system_abf_v2.md) | ✅ import + ABF 序列化 |
| [`docs/incremental_caas.md`](docs/incremental_caas.md) | ✅ 增量编译 |
| [`docs/roadmap.md`](docs/roadmap.md) | ✅ 完整路线图 |
| [`docs/known_issues.md`](docs/known_issues.md) | ✅ 活跃问题追踪 |

---

## 设计哲学

1. **最小核心** — 只保留最少的语义原语，上层语法都是宏。
2. **代码即数据** — Homoiconic，AI 直接操作 AST。
3. **自然生长** — 从真实语义需求中慢慢长大，不拔苗助长。
4. **AI 优先** — 默认动态类型 + 渐进加固；工具优先于生成。
5. **自进化** — 变异操作有类型保证，进化可审计可回滚。

---

## License

Apache 2.0
