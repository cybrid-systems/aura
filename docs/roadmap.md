# Aura — 实现进度跟踪

**构建方法**：《An Incremental Approach to Compiler Construction》（Ghuloum, ICFP 2006）
**现状**：已超出原论文 37 步的范围（查询引擎 + 反射），但语言核心有跳跃。

---

## 里程碑状态

```
M0 Racket原型    ✅  #lang aura + 全语义求值器 + ABF 序列化
M1 C++ 求值器   ✅  树遍历器 + IR 管线 + 26 模块 (Ghuloum Step 1-8)
M2 查询引擎     ✅  Query/Transform/AutoFix/HotSwap/--serve
M3 反射         ✅  P2996 auto_to_json + schema + 运行时内省
M3a 语言补全    🔨  布尔/序对/begin/set!/quote/cond (Ghuloum Step 9-13)
M3b 宏系统      ⬜  defmacro + 卫生展开 + 编译期验证
M4 生产         ⬜  LLVM JIT / AOT / 类型系统 / 自举
```

---

## Ghuloum 步骤对照

```
Step  C++    Racket   特性
────  ────   ──────   ──────────────────────────────
1     ✅     ✅       整数字面量
2     ✅     ✅       变量引用
3     ✅     ✅       lambda + 函数应用
4     ✅     ✅       if 条件
5     ✅     ✅       let 绑定
6     ✅     ✅       letrec 递归绑定
7     ✅     ✅       算术原语 (+ - * /)
8     ✅     ✅       比较 (= < > <= >=)
── 语言核心闭合 (Sprint B 红线) ──
9     🔨     ✅       布尔值 (not and or eq?)
10    🔨     ✅       序对 (cons car cdr null? pair?)
11    🔨     ✅       begin 顺序
12    🔨     ✅       set! 赋值
13    🔨     ✅       quote + 字面数据
14    🔨     ✅       cond 语法糖
── 语言完备 (Phase 3a 红线) ──
15    ⬜     ✅       defmacro 宏定义
16    ⬜     ✅       卫生宏 (gensym)
17+   ⬜     —       编译期 AST 验证 / 查询引擎 / 反射
```

---

## Phase 3a: 语言补全 — Ghuloum Step 9-13

| Step | Day | 新增 | 红线 |
|------|-----|------|------|
| 9 | 1 | 布尔值 (`not`, `and`, `or`, `eq?`) 作为 primitives | `(not #f)` → `1` |
| 10 | 1 | 序对 (`cons`, `car`, `cdr`, `null?`, `pair?`) | `(car (cons 1 2))` → `1` |
| 11 | 2 | `begin` 顺序执行 | `(begin 1 42)` → `42` |
| 12 | 2 | `set!` 赋值 | `(begin (set! x 10) x)` → `10` |
| 13 | 2 | `quote` + `cond` | `(quote (1 2))` / `(cond (#f 0) (1 42))` |

---

## M1-M3 回顾

### M1 — C++ 求值器 ✅

| 组件 | 状态 |
|------|------|
| CMake 4.0 + C++26 模块骨架 | ✅ |
| CLI 文本模式 + REPL | ✅ |
| ABF v2 反序列化 | ✅ |
| pmr 内存池 (ASTArena) | ✅ |
| CompilerService | ✅ |
| 树遍历器 (Expr*) | ✅ |
| 扁平 AST + SoA (FlatAST) | ✅ |
| AuraIR (24 opcodes) | ✅ |
| IR Lowering + 解释器 | ✅ |
| 闭包变换 + letrec | ✅ |
| PassManager | ✅ |
| compute-kind / arity / const-fold | ✅ |

### M2 — 查询引擎 ✅

| 组件 | 状态 |
|------|------|
| ASTIndex — SoA 过滤 | ✅ |
| QueryEngine — S-表达式查询 | ✅ |
| TransformEngine — Patch 生成 | ✅ |
| SymRefIndex — 符号引用倒排 | ✅ |
| Hot swap — 函数级 IR 替换 | ✅ |
| AutoFixEngine — 自动修复 | ✅ |
| --serve 模式 | ✅ |
| Racket Agent Demo | ✅ |

### M3 — 反射 ✅

| 组件 | 状态 |
|------|------|
| GCC 16.1 源码构建 | ✅ |
| P2996 auto_to_json<T>() | ✅ |
| Header-only reflect.hh | ✅ |
| aura-reflect 独立工具 | ✅ |
| Compile-time JSON Schema | ✅ |
| P1306 expansion demo | ✅ |
| 运行时闭包内省 | ✅ |
| --inspect / --env CLI | ✅ |
| E2E ABF 管线 | ✅ |
| #lang aura 恢复 | ✅ |

### M3b — 宏系统 (规划)

| 组件 | 状态 |
|------|------|
| defmacro 解析器 | ⬜ |
| 模板替换展开 | ⬜ |
| 卫生宏 (gensym) | ⬜ |
| 编译期 AST 验证 | ⬜ |

### M4 — 生产化 (规划)

| 组件 | 状态 |
|------|------|
| 三层运行时 (解释/JIT/AOT) | ⬜ |
| LLVM ORC JIT | ⬜ |
| 类型系统 (Sound Gradual Typing) | ⬜ |
| 自举 | ⬜ |
| CI/CD | ⬜ |

---

## 测试

```
CTest: 36 tests (1 pre-existing failure: test_ir nodiscard warning)
  - 9 step tests       (语言语义)
  - 1 ir_basic         (IR 管线)
  - 9 IR mode tests    (--ir flag)
  - 3 CLI query tests
  - 3 --serve tests
  - 2 inspect tests
  - 4 reflect/schema tests
  - 2 schema tests
  - 2 auto-fix tests
```
