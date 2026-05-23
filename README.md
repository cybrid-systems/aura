# Aura

**AI-native Lisp——让代码自己进化。**

---

我们不再让 AI 写代码。  
我们让 AI 拥有改写代码的权力——  
它可以读、可以改、可以让代码编译成独立的二进制。

---

## 核心工作流

### 1. 增量 EDSL — 代码第一次有了记忆

```scheme
;; serve session：代码在 workspace AST 中存活
(set-code "(define (f n) (* n 2))")
(query:find "f")                     → 找到自己的函数定义
(mutate:rebind "f" "(lambda (n) (* n 3))")  → AST 级替换
(eval-current)                       → 增量编译 + 求值
(current-source)                     → 反序列化完整源码
```

AI 不再猜测代码——它直接修改源码的 AST。  
错误不是终点，而是下一轮迭代的起点。

### 2. 蚁群自搜索 — colony:search

一次 `serve.exec()` 完成全部本地搜索：

```scheme
(colony:search "42" 20)
  → 扫描 workspace，找到所有命名函数
  → 对每个函数尝试 display-ref / lit-tweak 变异
  → 用 eval-current-output 捕获输出
  → 返回 (#t 输出 描述)
```

LLM 做方向，Aura 做局部搜索。信息素指引变异优先级。

### 3. 冻成原生二进制

```scheme
;; 进化完成后，一行命令出独立 ELF
(write-file "/tmp/prog.aura" (current-source))
(shell (string-append "./build/aura --emit-binary /tmp/myapp ..."))
```

或直接：

```bash
# 从源码生成原生二进制
echo '(display (+ 1 2))' | ./build/aura --emit-binary myapp
./myapp  # → 3，不需要 aura 本体

file myapp
# ELF 64-bit LSB executable, ARM aarch64
```

支持 arm64 和 x86_64（通过 `AURA_ARCH` 环境变量）。

---

## 技术速览

Aura 是一个 **AI-native Lisp** 编译器：C++26 实现，LLVM ORC JIT 后端，Sound Gradual Typing。

### 语言能力

| 维度 | 状态 |
|------|:----:|
| **核心求值** | Tree-walker + IR 双路径 + TCO + 显式调用栈 |
| **类型系统** | Sound Gradual: coercion + occurrence + let-poly + blame + type query |
| **M4 线性所有权** | move/borrow/drop, 编译期跟踪 + IR opcode + 运行时 |
| **ADT + match** | define-type / 穷尽性检查 ✅ |
| **JIT** | ORC JIT, 38 opcode → native, 7.55× vs TW, -O2 |
| **增量编译** | ArenaGroup + 磁盘缓存 + 热替换 + IR import |
| **EDSL 自修改** | set-code → query → mutate → eval-current → colony:search |
| **模块系统** | require/import + 路径解析 + 缓存 + 循环检测 + 热重载 ✅ |
| **标准库** | 29 模块：string/list/hash/math/json/csv/io/socket/datetime/vector-math |
| **C FFI** | dlopen/dlsym + 类型签名 `"(String) -> Int"` |
| **TCP 网络** | tcp-connect/send/recv/close, http-get/post |
| **文件 I/O** | read-file/write-file/file-exists?/file-copy/file-delete/directory-list |
| **进程** | shell/command-output/command-line |
| **错误处理** | try-catch + 结构化诊断 (ErrorKind + BlameInfo) |
| **编译期反射** | P2996 auto_to_json / auto_serialize / P1306 递归序列化 |
| **原生二进制** | --emit-binary → 独立 ELF (arm64/x86_64) |

### AI 基准（99 生成任务，2026-05-23）

| 模型 | 通过率 | 耗时 |
|:----|:------:|:----:|
| 🥇 Grok 4.3 | **93/102 (91.2%)** | ~9min |
| 🥈 DeepSeek v4 Flash | **87/102 (85.3%)** | ~7min |
| 🥉 MiniMax M2.7 | **47/102 (46.1%)** | ~13min |

> MiniMax-M2.7 是推理模型，强制输出 `<think>` 标签。通过 `reasoning_split=True` 分离推理层后从 47→56/102。但仍因模型定位差异（推理 > 代码生成），分数远低于 Grok/DeepSeek。

[详情 → docs/benchmark.md](docs/benchmark.md)

### 快速开始

```bash
# 编译
cmake -B build && cmake --build build --target aura -j

# 运行
echo '(+ 1 2 3)' | ./build/aura                    # → 6
echo '(display 42)' | ./build/aura                 # → 42

# 类型注解
echo '((lambda ((: x Int)) (+ x 1)) 41)' | ./build/aura  # → 42

# 增量 EDSL
echo '(set-code "(define (f x) (+ x 1))(display (f 41))")(eval-current)' | ./build/aura

# 冻成原生二进制
echo '(display (+ 1 2))' | ./build/aura --emit-binary myapp
./myapp  # → 3
```

```bash
# AI 驱动测试
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --max-attempts 5
```

---

## 文档

- [docs/tutorial.md](docs/tutorial.md) — 10 分钟入门
- [docs/design/aura_language_spec.md](docs/design/aura_language_spec.md) — 语法规格
- [docs/roadmap.md](docs/roadmap.md) — 路线图及能力评估
- [docs/known_issues.md](docs/known_issues.md)
- [design repo](https://github.com/cybrid-systems/ai-programming-language-design)

## License

Apache 2.0
