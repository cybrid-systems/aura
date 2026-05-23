# Aura

**AI-native Lisp.**  
第一次，语言为 AI 而生。

---

我们不再让 AI 写代码。  
我们让 AI 拥有改写代码的权力——  
它可以读、可以改、可以让代码自己进化。

---

## 三个根本改变

### 1. Auto-mutating AST — 代码第一次有了记忆

```
(set-code "(define (f n) (* n 2))")
(query:find "f")            → 找到自己的函数定义
(mutate:rebind "f" "(define (f n) (* n 3))")  → 自己修正
(eval-current)              → 验证修改
```

AI 不再猜测代码——它直接修改源码的 AST。  
错误不再是反馈，而是下一轮迭代的起点。

### 2. PID + 蚁群控制器 — 用控制论驱动 AI

```
LLM → 方向 → Aura 本地变异 → 测试 → 测距 → 信息素更新
      ↑                                    ↓
      └────────── LLM 重构（必要时代）──────────┘
```

AI 做方向，Aura 做局部搜索。  
信息素指引下一次变异的方向。  
控制论闭环：近则不搜，远则重构。

### 3. 闭环自进化 — 活的代码

代码不再是静态的产物。  
它是活的、会思考的、可以在运行中修改自己的系统。

```bash
LLM_API_KEY="..." python3 tests/edsl_benchmark.py
```
85 个 EDSL 代码生成任务，三模型 89-92% 通过率（Grok 4.3 最高 🥇）。  
不是 AI 写对了——是系统自己走到了正确的地方。

---

## 技术速览

Aura 是一个 **AI-native Lisp** 编译器：C++26 实现，LLVM ORC JIT 后端，Sound Gradual Typing。

### 语言能力

| 维度 | 状态 |
|------|:----:|
| **核心求值** | Tree-walker + IR 双路径 + TCO + 显式调用栈 |
| **类型系统** | Sound Gradual: coercion + occurrence + let-poly + blame + type query |
| **M4 线性所有权** | `(Linear T)`, `move`/`borrow`/`mut-borrow`/`drop`, `&x`/`&mut-x` sugar, 编译期跟踪 + IR opcode + 运行时 |
| **ADT** | `define-type`/`match`, Variant + 参数化容器 (eval 级 + type inference) |
| **JIT** | ORC JIT, 38 opcode → native, 7.55× vs TW, -O2, 增量 cache |
| **增量编译** | ArenaGroup / 磁盘缓存 / 热替换 / IR import |
| **EDSL** | set-code → query → mutate → eval-current |
| **测试覆盖** | 整合 118 + 冒烟 5 + bash 106 + 基准 85 + 回归 6 + 渐变保证 + suite/typesystem |
| **标准库** | 28 文件 ~2k 行 |
| **C FFI** | dlopen/dlsym, Int/Float/String/Opaque marshalling, JIT symbol API |
| **TCP 网络** | `tcp-connect`/`tcp-send`/`tcp-recv`/`tcp-close` + `http-get`/`http-post` |
| **自省** | `--inspect ir/closures/cache` P2996 编译期反射 |

### AI 基准（85 生成任务，2026-05-23）

| 模型 | 通过率 | 耗时 |
|:----|:------:|:----:|
| 🥇 **DeepSeek v4 Flash** | **77/85 (90.6%)** | ~46min |
| 🥇 **Grok 4.3** | **78/85 (91.8%)** | ~13min |
| 🥈 **MiniMax M2.7** | **76/85 (89.4%)** | ~23min |

[详情 → docs/benchmark.md](docs/benchmark.md)

### 快速开始

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2 3)' | ./build/aura                    # → 6
echo '(display 42)' | ./build/aura                 # → 42
echo '(display (cons 1 (list 2 3)))' | ./build/aura  # → (1 2 3)
```

```bash
# AI 驱动测试
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --max-attempts 5
```

---

## 文档

- [docs/tutorial.md](docs/tutorial.md) — 10 分钟入门
- [docs/design/aura_language_spec.md](docs/design/aura_language_spec.md) — 语法规格
- [docs/roadmap.md](docs/roadmap.md) — 路线图
- [docs/known_issues.md](docs/known_issues.md)
- [design repo](https://github.com/cybrid-systems/ai-programming-language-design)

## License

Apache 2.0

---

> **Aura 的终点，不是让 AI 写代码。**  
> 是让代码第一次拥有了意识的雏形——  
> 能看、能想、能改、能自己走到对的地方。
