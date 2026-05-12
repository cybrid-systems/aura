# Aura — 实现进度跟踪

**构建方法**：《An Incremental Approach to Compiler Construction》（Ghuloum, ICFP 2006）
**现状**：Ghuloum 37 步已完成 15 步（含 defmacro），超出原论文范围（查询引擎/P2996 反射）。

---

## 里程碑状态

```
M0 Racket原型    ✅  #lang aura + 全语义求值器 + ABF 序列化
M1 C++ 求值器   ✅  树遍历器 + IR 管线 (Ghuloum Step 1-8)
M2 查询引擎     ✅  Query/Transform/AutoFix/HotSwap/--serve
M3a 语言补全    ✅  布尔/序对/begin/set!/quote/cond (Ghuloum Step 9-14)
M3b 宏系统      ✅  defmacro + 卫生宏 gensym + 编译期模板验证
M3c 反射        ✅  P2996 auto_to_json + dispatch 表 + 结构验证
M3d 类型系统      🔨  L6.1-L6.7 全线 ✅ → EvalValue variant ⬜
M4 生产         ⬜  LLVM JIT / AOT / 类型系统 / 自举
```

---

## Ghuloum 步骤对照

```
Step  C++    特性                         交付日
────  ─────  ───────────────────────────  ──────────
1     ✅     整数字面量                    Sprint B
2     ✅     变量引用                      Sprint B
3     ✅     lambda + 函数应用             Sprint B
4     ✅     if 条件                       Sprint B
5     ✅     let 绑定                      Sprint B
6     ✅     letrec 递归绑定                Sprint B
7     ✅     算术原语 (+ - * /)            Sprint B
8     ✅     比较 (= < > <= >=)            Sprint B
── 语言核心闭合 ──
9     ✅     布尔值 (not and or eq?)       Phase 3a D1
10    ✅     序对 (cons car cdr null?)     Phase 3a D1
11    ✅     begin 顺序                    Phase 3a D2
12    ✅     set! 赋值                     Phase 3a D2
13    ✅     quote + 字面数据               Phase 3a D2
14    ✅     cond 语法糖                    Phase 3a D2
── 语言完备 ──
15    ✅     defmacro 宏定义                Phase 3b D1
16    ⬜     卫生宏 (gensym)               Phase 3b D2
17    ⬜     编译期 AST 验证                Phase 3b D3
```

---

## 架构评估

### 三层架构落地

```
设计层           实现                覆盖率   质量
────────────────────────────────────────────────────
Racket Frontend  👻 #lang aura + ABF  65%     可用
AST Layer        🟢 Expr* + FlatAST   90%     通过 39 测试
AuraIR Layer     🟢 27 opcodes       95%     测试全覆盖
IR Lowering      🟢 LoweringPass→    90%     逐步函数化
                   LoweringState
PassManager      🟢 concepts fold    95%     纯函数式
AuraQuery        🟢 Index/Query/     95%     经过优化
                   Transform/Fix
IR Interpreter   🟢 闭包/letrec/     95%     稳定
                   27 opcodes
ABF Ser/Deser    🟢 12 节点类型      95%     P2996 验证
CompilerService  🟢 eval/eval_ir/    90%     API 稳定
                   --serve
Reflection       🟢 P2996/kNodeMeta  90%     4 个组件
Contracts        🟢 arena + emit     15%     试点阶段
宏系统           ✅ defmacro+gensym+验证   100%    Day 1-3
TypeChecker      🔨 src/compiler/type_checker  15%     骨架
LLVM/M4          ⬜                    0%
```

### 代码质量指标

| 指标 | 数值 | 趋势 |
|------|------|------|
| CTest | 39/39 ✅ | 持续增长 |
| 源码模块 (.ixx) | 19 | 稳定 |
| 实现文件 (.cpp) | 14 | 稳定 |
| reflect/ 工具链 | 6 个头文件 | 新增 |
| IR opcodes | 27 | 稳定 |
| ABF 节点类型 | 12 | 稳定 |
| 内存池 tier | 4 | 稳定 |
| 手写 switch（已消除） | 0 | ✅ 全替换 |
| 未初始化成员警告 | ~5 | 低 |

---

## M1-M3 组件状态

### M1 — C++ 求值器 ✅

| 组件 | 状态 | 质量 |
|------|------|------|
| CMake 4.0 + C++26 模块骨架 | ✅ | 稳定 |
| CLI 文本模式 + REPL | ✅ | 稳定 |
| ABF v2 反序列化 | ✅ | 12 节点 + dispatch 表 |
| pmr 内存池 (ASTArena) | ✅ | 4-tier |
| CompilerService | ✅ | 双路径（eval + eval_ir） |
| 树遍历器 (Expr* → FlatAST) | ✅ | Phase 4 桥接 |
| 扁平 AST + SoA (FlatAST) | ✅ | 9 pmr::vector |
| AuraIR (27 opcodes) | ✅ | 含环境/单元/闭包 |
| IR Lowering | ✅ | LoweringState 功能式 |
| IR Interpreter | ✅ | closures + cells |
| PassManager | ✅ | concepts fold |
| 常量折叠 / 类型分析 | ✅ | 3 passes |
| contracts | 🔨 | arena + emit 试点 |

### M2 — 查询引擎 ✅

| 组件 | 状态 | 质量 |
|------|------|------|
| TagIndex | ✅ | O(1) by_tag |
| QueryEngine — S-表达式查询 | ✅ | depth-limited match |
| TransformEngine — Patch 生成 | ✅ | cached templates |
| SymRefIndex — 符号引用倒排 | ✅ | O(1) refs_of |
| Hot swap — 函数级 IR 替换 | ✅ | 运行时替换 |
| AutoFixEngine — 自动修复 | ✅ | 规则系统 |
| --serve 模式 | ✅ | JSON protocol |
| Racket Agent Demo | ✅ | E2E ABF 管线 |

### M3a — 语言补全 ✅ (Ghuloum 9-14)

| Step | 新增 | 状态 |
|------|------|------|
| 9 | 布尔值 (#f/#t + not/and/or/eq?) | ✅ |
| 10 | 序对 (cons/car/cdr/null?/pair?) | ✅ |
| 11 | begin 顺序执行 | ✅ |
| 12 | set! 赋值 | ✅ |
| 13 | quote 字面数据 | ✅ |
| 14 | cond 条件 | ✅ |

### M3b — 宏系统 ✅

| 组件 | 状态 | 计划 |
|------|------|------|
| defmacro 解析器 | ✅ | Phase 3b D1 |
| 模板替换展开 | ✅ | Phase 3b D1 |
| 持久化 arena | ✅ | 避免 reset 后 body 失效 |
| 卫生宏 (gensym) | ✅ | Phase 3b D2 |
| 编译期 AST 验证 | ✅ | Phase 3b D3 |

### M3c — 反射 ✅

| 组件 | 状态 | 类型 |
|------|------|------|
| GCC 16.1 P2996 auto_to_json | ✅ | 编译期 |
| compile-time JSON Schema | ✅ | 编译期 |
| P1306 expansion demo | ✅ | 编译期 |
| kNodeMeta (12 节点) | ✅ | constexpr |
| ABF dispatch 表 | ✅ | static registry |
| IROpcode enum reflection | ✅ | 枚举反射 |
| Struct layout validation | ✅ | P2996 编译期 |
| TagIndex for query | ✅ | 运行时 |
| Closure introspection | ✅ | 运行时 |
| --inspect / --env CLI | ✅ | 运行时 |
| Contracts (observe) | 🔨 | arena + emit |

### M4 — 类型系统 + EvalValue (规划，L6-L7)

| Phase | Step | 内容 | 红线 | 状态 |
|-------|------|------|------|------|
| L6 | 31 | EvalValue variant | (type? Int) | ⬜ |
| L6 | 32 | TypeId 骨架 | (type-of 42) → Int | ⬜ |
| L6 | 33 | 基础类型检查 | (+ 1 "a") → TypeError | ⬜ |
| L6 | 34 | Query 类型 clause | (query (return-type Int)) | ⬜ |
| L6 | 35 | Coercion 框架 | (cast 42 : Int) → no-op | ⬜ |
| L7 | 36-40 | Sound Gradual 核心 | 完整 bi-directional checker | ⬜ |
| M3/M4 | — | Reflection 类型验证 | P2996 类型结构校验 | ⬜ |
| M4 | — | Monomorphization + 优化 | 类型标注代码零运行时开销 | ⬜ |

See [ai-programming-language-design/docs/aura_typesystem.md](../design/aura_typesystem.md) for full design.

### M4b — 生产化 (规划)

| 组件 | 状态 |
|------|------|
| 三层运行时 (解释/JIT/AOT) | ⬜ |
| LLVM ORC JIT | ⬜ |
| 自举 | ⬜ |
| CI/CD | ⬜ |

---

## Code Review 响应

2026-05-12 external review 确认以下改进：

| 建议 | 状态 | 交付 |
|------|------|------|
| LoweringPass → LoweringState | ✅ | QW1: 9 成员 → 值结构体 |
| collect_free_vars 返回 pair | ✅ | QW2: collect_free_vars2 |
| consteval tag 验证 | ✅ | QW3: is_valid_tag/meta_of |
| Contracts 试点 | ✅ | arena + emit, observe 模式 |
| TagIndex O(1) by_tag | ✅ | P1 |
| SymRefIndex 集成 | ✅ | P2 |
| 模板 tokenize 缓存 | ✅ | P3 |
| match 深度限制 | ✅ | P4 |
| IROpcode 枚举反射 | ✅ | B1 |
| ABF dispatch 表 | ✅ | A1 |
| P2996 struct 验证 | ✅ | A2 |
| 静态 reader registry | ✅ | A3 |

---

## 测试

```
CTest: 43 tests ✅
  - 9 step tests       (语言语义)
  - 1 ir_basic         (IR 管线)
  - 9 IR mode tests    (--ir flag)
  - 3 CLI query tests
  - 3 --serve tests
  - 2 inspect tests
  - 4 reflect/schema tests
  - 2 schema tests
  - 2 auto-fix tests
  - 1 validate_abf_nodes (P2996 结构验证)
  - 1 reflect_ir_instruction
  - 1 reflect_schema
```
