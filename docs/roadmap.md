# Aura — 实现进度跟踪

> 完整三轨并行路线图见设计仓库：
> **[docs/aura_roadmap.md](https://github.com/cybrid-systems/ai-programming-language-design/blob/main/docs/aura_roadmap.md)**
>
> 本文档跟踪每一步在 Aura 实现仓库中的实际完成状态。

---

## 里程碑状态

```
M0 种子     ✅ 已完成 (Racket #lang 原型)
M1 C++求值  🔨 当前 (Step A1.1 / L1.1 / I1.1)
M2 管线     ⬜
M3 查询     ⬜
M4 反射     ⬜
M5 生产     ⬜
```

---

## 当前里程碑：M1 — C++ 求值器

### 🏗 架构 — Compiler Service 骨架

| Step | 特性 | 红线 | 状态 |
|------|------|------|------|
| A1.1 | CMake + 模块骨架 | `cmake -B build && cmake --build build` | ✅ |
| A1.2 | Compiler Service 进程框架 + 文本解析器 | `echo '(+ 1 2)'` → 不崩溃 | ⬜ |
| A1.3 | ABF 序列化器 (Racket 端) | `(serialize-expr '(+ 1 2))` → bytes | ⬜ |
| A1.4 | ABF 反序列化器 (C++ 端) | Racket ABF → C++ 结构等价 | ⬜ |
| A1.5 | 共享内存传输层 | Racket → mmap → C++ → eval → 结果 | ⬜ |

### 🗣 语言 — C++ Tree-Walker

| Step | 特性 | 红线 | 状态 |
|------|------|------|------|
| L1.1 | 整数字面量 | `echo 42 | ./aura` → `42` | ✅ |
| L1.2 | 变量 + 环境 | `echo x \| ./aura --env 'x=10'` → `10` | ⬜ |
| L1.3 | 算术原语 | `'echo (+ 1 (* 2 3))'` → `7` | ⬜ |
| L1.4 | if | `'echo (if (> 3 2) 1 0)'` → `1` | ⬜ |
| L1.5 | 闭包 + 函数应用 | `'echo ((lambda (x) (* x 2)) 5)'` → `10` | ⬜ |
| L1.6 | let + letrec | `'echo (letrec ... (fact 5))'` → `120` | ⬜ |
| L1.7 | Hyperstatic define | define → eval 一致 | ⬜ |
| L1.8 | C++ REPL | `./aura` 交互式 | ⬜ |

### 🔧 基建 — 构建 + 测试

| Step | 特性 | 红线 | 状态 |
|------|------|------|------|
| I1.1 | CTest 基础测试框架 | `ctest --test-dir build` → 3/3 通过 | ✅ |
| I1.2 | 混合构建 (Racket + C++) | `make && make test` | ⬜ |
| I1.3 | CI 管线 | PR → CI 自动跑测试 | ⬜ |
| I1.4 | 性能基准框架 | `./benchmark` → 可记录 | ⬜ |
| I1.5 | 回归测试自动化 | `python regress.py` → ALL PASS | ⬜ |

---

## 验证方式

每完成一个 Step，在 `tests/` 下增加该步骤的集成测试：

```bash
ctest --test-dir build  # C++ 端测试
python regress.py       # 全回归检查
```

每条红线就是测试用例。红线通过 = Step 完成。

---

## 相关文档

- 完整路线图：[docs/aura_roadmap.md](https://github.com/cybrid-systems/ai-programming-language-design/blob/main/docs/aura_roadmap.md)
- 设计文档：[docs/](https://github.com/cybrid-systems/ai-programming-language-design/tree/main/docs/)
