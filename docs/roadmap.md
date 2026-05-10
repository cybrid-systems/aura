# Aura — 实现进度跟踪

> 完整 Ghuloum 增量构建路线图见设计仓库：
> **[docs/aura_roadmap.md](https://github.com/cybrid-systems/ai-programming-language-design/blob/main/docs/aura_roadmap.md)**
>
> 本文档跟踪每一步在 Aura 实现仓库中的实际完成状态。

---

## 当前状态

**Phase 0 (Racket #lang 原型)**: ✅ 已完成
**Phase 1a (C++26 最小求值器)**: 🔨 Step 09 — 编译器骨架 + 整数求值

---

## Phase 0：Racket 原型 (已完成)

| Step | 特性 | 红线 | 状态 | 提交 |
|------|------|------|------|------|
| 01 | 整数字面量 | `(eval 42)` → `42` | ✅ | — |
| 02 | 变量引用 | `(eval 'x '{x 10})` → `10` | ✅ | — |
| 03 | lambda + 应用 | `(eval '((lambda (x) x) 1))` → `1` | ✅ | — |
| 04 | if 条件 | `(eval '(if #t 1 2))` → `1` | ✅ | — |
| 05 | let + letrec | `(eval '(let ((x 5)) x))` → `5` | ✅ | — |
| 06 | quote + 数据 | `(eval '(quote (a b)))` → `(a b)` | ✅ | — |
| 07 | Hyperstatic define | `(eval '(define x 5) env)` | ✅ | — |
| 08 | REPL 循环 | `racket -l aura` → 交互 | ✅ | — |

> 注意：Phase 0 之前完成过并留下编译产物，但源码未入 git。下次开工时需从 Step 01-08 重建 Racket #lang。

---

## Phase 1a：C++26 最小求值器

| Step | 特性 | 红线 | 状态 | 计划完成 |
|------|------|------|------|---------|
| 09 | 骨架 + 整数求值 | `./aura --eval 42` → `42` | 🔨 | 本周 |
| 10 | 变量引用 + 环境 | `./aura --eval '(let ((x 10)) x)'` → `10` | ⬜ | Week 1 |
| 11 | 算术原语 | `./aura --eval '(+ 1 (* 2 3))'` → `7` | ⬜ | Week 2 |
| 12 | 条件分支 | `./aura --eval '(if (> 3 2) 1 0)'` → `1` | ⬜ | Week 2 |
| 13 | 闭包 + 函数调用 | `./aura --eval '((lambda (x) (* x 2)) 5)'` → `10` | ⬜ | Week 2 |
| 14 | let / letrec | `./aura --eval '(letrec ... (fact 5))'` → `120` | ⬜ | Week 3 |
| 15 | Hyperstatic define | `./aura' 中 (define x 5) → (eval 'x)` → `5` | ⬜ | Week 3 |
| 16 | REPL 循环 | `./aura` 交互式 → 可用 | ⬜ | Week 3 |

---

## 验证方式

每完成一个 Step，在 `tests/` 下增加对应的集成测试：

```bash
# Step 09 的测试
(cd tests && raco test)  # Racket 端测试
ctest --test-dir build   # C++26 端测试
```

每条红线同时是测试用例。红线通过 = Step 完成。

---

## 相关文档

- 完整路线图：[docs/aura_roadmap.md](https://github.com/cybrid-systems/ai-programming-language-design/blob/main/docs/aura_roadmap.md)
- 设计文档：[docs/](https://github.com/cybrid-systems/ai-programming-language-design/tree/main/docs/)
