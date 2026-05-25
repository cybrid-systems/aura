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

### 3. 冻成原生二进制（AOT）

`--emit-binary` 走完整 AOT 管线：LLVM IR → llc → 链接 → ELF。

```bash
# 多文件 / 内联 / 管道，一行出独立 ELF
./build/aura --emit-binary lib.aura main.aura app    # 多文件
./build/aura --emit-binary '(+ 1 2)' app               # 内联
echo '(+ 1 2)' | ./build/aura --emit-binary app       # 管道
./app  # → 3，不依赖 aura 本体
```

**支持的表达式：** `+ - * / = < > <= >= and or not if pair? null? cons car cdr list length reverse append member map filter foldl string-length string=? string-append string-ref number->string quot/rem display let lambda error gensym`

**闭包 + 高阶函数：** 用户 lambda 编译为 ELF 函数，`map`/`filter`/`foldl` 通过原语派发表调用用户函数。原语 `+ - * / = < > <= >= not` 可作为闭包值传递（`(let ((f +)) (f 1 2 3))`）。

**输出：** raw int64_t（`1` = #t, `0` 不输出）。display 自动换行，不重复打印返回值。

**架构：** arm64 / x86_64。

→ 48 emit 测试，详见 [docs/roadmap.md](docs/roadmap.md)

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
| **原生二进制 (AOT)** | LLVM IR → llc → 链接 → ELF. 算术/比较/逻辑/列表/字符串/闭包/高阶函数/所有权/多文件 (48 tests) |

### AI 基准（99 生成任务，2026-05-23）

| 模型 | 通过率 | 耗时 |
|:----|:------:|:----:|
| 🥇 Grok 4.3 | **93/111 (83.8%)** | ~50s |
| 🥈 DeepSeek v4 Flash | **92/111 (82.9%)** | ~173s |

> P1 编译器加固 (M4 borrow checker / closure cache / inline prim / occurrence typing / 事务 rollback) 后，DeepSeek 从 87/102 升至 92/111 (+5↑)。

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
