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
(set-code "(define (f n) (* n 2))")
(query:find "f")                     → 找到自己的函数定义
(mutate:rebind "f" "(lambda (n) (* n 3))")  → AST 级替换
(eval-current)                       → 增量编译 + 求值
```

### 2. 冻成原生二进制（AOT）

```bash
# 多文件 / 内联 / 管道，一行出独立 ELF
./build/aura --emit-binary lib.aura main.aura app   # 多文件
./build/aura --emit-binary '(+ 1 2)' app             # 内联
echo '(+ 1 2)' | ./build/aura --emit-binary app     # 管道
./app  # → 3，不依赖 aura 本体
```

**编译管线：** `源码 → FlatAST → IRModule → LLVM IR (O2) → .ll → llc → .o → 链接 runtime.c → ELF`

**支持：** 算术/比较/逻辑/类型/对/列表/字符串/高阶函数/闭包+递归/apply/display(列表格式化)/所有权/多文件/stdlib 集成。

```scheme
;; 全部可在原生 ELF 中运行
(+ 1 2 3) | (= 42 42) | (and #t #t)
(car (list 1 2 3)) | (length (list 1 2 3))
(string-length "hello") | (string-append "a" "b")
(map (lambda (x) (+ x 10)) (list 1 2 3))
(foldl + 0 (list 1 2 3)) | (apply + (list 1 2 3))
(let loop ((x 0)) (if (< x 3) (loop (+ x 1)) x))
(display (list 1 2 3))   → (1 2 3)
(import "std/math")(factorial 5) → 120
(import "std/algorithm")(permutations (list 1 2 3))
```

**56 emit 测试全通过。** 架构：arm64 / x86_64。

---

## 技术速览

Aura：C++26，LLVM ORC JIT 后端，Sound Gradual Typing。

| 维度 | 状态 |
|------|:----:|
| **核心求值** | Tree-walker + IR 双路径 + TCO + 显式调用栈 |
| **类型系统** | Sound Gradual: coercion + occurrence + let-poly |
| **M4 线性所有权** | move/borrow/drop, 编译期跟踪 + IR opcode |
| **ADT + match** | define-type / 穷尽性检查 |
| **JIT** | ORC JIT, 38 opcode → native, 7.55× vs TW, -O2 |
| **增量编译** | ArenaGroup + 磁盘缓存 + 热替换 + IR import |
| **EDSL 自修改** | set-code → query → mutate → eval-current |
| **模块系统** | require/import + 路径解析 + 缓存 + 热重载 |
| **原生二进制 (AOT)** | LLVM IR → llc → 链接 → ELF. 算术/比较/闭包递归/高阶/apply/列表/字符串/所有权/多文件/stdlib (56 emit ✅) |
| **C FFI** | dlopen/dlsym + 类型签名 |
| **编译期反射** | P2996 auto_to_json / auto_serialize |

### 快速开始

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2)' | ./build/aura                 # → 3
echo '(display (list 1 2 3))' | ./build/aura  # → (1 2 3)
echo '(+ 1 2)' | ./build/aura --emit-binary myapp && ./myapp  # → 3
```

## 文档

- [docs/roadmap.md](docs/roadmap.md)
- [docs/known_issues.md](docs/known_issues.md)
- [docs/design/aura_language_spec.md](docs/design/aura_language_spec.md)

## License

Apache 2.0
