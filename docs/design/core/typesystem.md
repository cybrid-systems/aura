# Aura 类型系统

**状态**: 渐进类型 L6 + T2a-T2e + T4 完整管线，集成到 IR 管线（warnings-only 模式）。T4 包括增量类型检查、Let-Poly、ADT 穷尽性检查。
**源码**: `src/compiler/type_checker_impl.cpp`, `src/core/type_impl.cpp`, `src/compiler/diag.ixx`

---

## 0. Implementation Status (2026-06-11, Issue #156)

**重要**：本文档的 **核心规则 / 渐进类型 / Occurrence Typing / 约束求解 / ADT / Blame / Value Restriction / DeadCoercionEliminationPass 全部实装**。T4（增量 / Let-Poly / 穷尽性）在 `synthesize_flat` 路径实装。准确分两层：

### C++ Core Layer (`src/compiler/type_checker_impl.cpp` / `src/core/type_impl.cpp` / `src/compiler/diag.ixx`)

| 模块 | 实装 | 备注 |
|------|------|------|
| `TypeChecker::infer_flat` / `check_flat` / `analyze_predicate_flat` | ✓ | 对外 API |
| `InferenceEngine::synthesize_flat` / `check_flat` / `is_coercible` | ✓ | bi-directional + AND/OR 谓词组合 |
| `TypeEnv` (作用域 + 类型绑定) | ✓ | 含 `is_poly` 动态决定（Let-Poly）|
| `ConstraintSystem` (Union-Find + multi-pass worklist) | ✓ | T2a — EQUAL / CONSISTENT 约束 |
| `TypeRegistry` (核心类型 + ADT 构造函数注册) | ✓ | T2b — 跨调用重建（~1ms 可接受）|

（Refactor 2.3/3.2 note）ADT 构造函数注册状态已从 evaluator_impl 全局移至 adt_runtime_ (FFI 模式提取完成); 旧 g_adt 已移除。详见 adt_runtime 模块 + evaluator.md File map。merr 消除 (3.1) 也与 error 返回相关代码同步。

（Phase 2 pilot-4 note）CMake test dedup 继续：test_issue_134 (ADT 验证) 已转为使用早期定义的 aura_add_issue_test helper + 小 append。helper 定义已提前保证顺序正确。详见 CMakeLists + evaluator.md §12。
| `OwnershipEnv` (线性所有权) | ✓ | M4 — 编译期跟踪 |
| 渐进类型（Dynamic + consistent_unify）| ✓ | `?` 与任意类型匹配 |
| Coercion (is_coercible + add_coercion + CastOp 插入) | ✓ | IR/JIT CastOp 扩展 type_tag 覆盖 Int/Float/Bool/String |
| Occurrence Typing 谓词分析 (`number?`/`string?`/`pair?`/`symbol?`/`float?`/`boolean?`) | ✓ | `analyze_predicate_flat` + AND/OR 组合 |
| 增量类型检查 (dirty skip via `FlatAST::is_dirty(id)`) | ✓ (T4) | `synthesize_flat` 路径实装，含 `cache_hits`/`cache_misses`/`stale_cache` 统计 |
| Let-Poly (`is_poly` + `instantiate_forall`) | ✓ (T4) | `TypeEnv::bind` + `lookup` 联动 |
| ADT 穷尽性检查 (`__match_tmp` + `get_adt_constructors`) | △ (T4, partial) | 单层 match 已实装；嵌套 match 独立检查未做（§5.5）|
| Blame 结构化 (BlameParty + BlameInfo + Consistent-subtype) | ✓ (T2c) | 含 Coercion marker pass |
| Value Restriction (syntactic_value) | ✓ | 防止非值 let 泛化 |
| `DeadCoercionEliminationPass` | ✓ (T2e) | IR 级 pass + Gradual Guarantee 10+ 测试 |
| 多 mutation 串行场景下的粒化增量 | 🟡 (§5.1) | 当前按单次 mutation 触发；可进一步粒化 |
| 类型信息流入 IR (除 CastOp 外) | 🔴 (§5.2) | `infer_flat` 返回 TypeId 在 `eval()` 里被忽略；类型驱动 IR 优化未做 |
| 嵌套 match 独立穷尽性检查 | 🔴 (§5.5) | 单层 match 实装；嵌套 case 未做 |
| 模块类型签名传播 | 🔴 (§5.4) | import/require 绑定大部分是 Dynamic |

### Aura Layer

类型系统是 **C++ 内部管线**，通过 `eval()` 隐式触发。Aura 端有 `query:type` 原语查询 AST 节点的推断类型（`TypeResolutionIndex`, M2.7），**没有**用户可调的类型注解 / 谓词原语。

### 已实现 vs 计划

- ✅ **实装**：T2a-T2e（约束求解 / 渐进类型 / Blame / Coercion marker / Dead Coercion 消除）+ T4（增量 / Let-Poly / 穷尽性单层）
- 🟡 **部分实装**：ADT 穷尽性（嵌套 match 待做）
- 🔴 **未做**：类型信息流入 IR（除 CastOp）；模块类型签名；嵌套 match 独立检查

**AI Agent 读者请注意**：类型检查是 **warnings-only** 模式（不阻塞执行）。AI Agent 修改代码后可调用 `query:type` 查询推断类型；`eval()` 触发类型检查但不阻止执行；写 Aura 代码时不要依赖类型检查的硬失败。

---


## 1. 架构总览

```
TypeChecker (对外 API)
 ├── infer_flat(FlatAST&, StringPool&, NodeId, DiagnosticCollector&) → TypeId
 ├── check_flat(FlatAST&, StringPool&, NodeId, TypeId expected)
 └── analyze_predicate_flat(FlatAST&, ...) — Occurrence Typing
       │
       ▼
InferenceEngine
 ├── synthesize_flat(...)  — 类型合成（自下而上）
 ├── check_flat(...)    — 类型检查（给定预期类型，bi-directional）
 ├── is_coercible(...)  — 类型可转换性判断
 └── analyze_predicate_flat(...) — Occurrence Typing 谓词分析 (含 and/or 组合)
       │
       ▼
TypeEnv            — 作用域 + 类型绑定
ConstraintSystem   — Union-Find 约束求解 + multi-pass worklist fixpoint
TypeRegistry       — 核心类型定义存储（持久，含 ADT 构造函数注册）
OwnershipEnv       — M4 线性所有权跟踪（编译期）
```

### 执行管线中的位置（更新）

```
eval(expr):
  parse → macro_expand → needs_fallback?
    ├── yes → eval_flat (tree-walker, 无类型检查)
    └── no  → TypeChecker.infer_flat()    ← 类型检查（warnings only）
               ↓
              lower_to_ir_with_cache()     ← 类型信息不向下传 (CastOp 例外)
               │
               ├── DeadCoercionEliminationPass  ← 消除冗余 CastOp
               ├── ComputeKind/Arity/ConstFold
               └── IRInterpreter / ORC JIT     ← 弱类型执行 (CastOp 运行时检查)
```

**关键决策**：类型检查是**非阻塞前置分析**。警告不影响执行，IR 引擎完全无类型。

## 2. 已实现的类型特性

### 2.1 核心规则

| 规则 | 源码位置 | 状态 |
|------|---------|------|
| T-Int / T-Float / T-Bool / T-String | `synthesize_flat` 字面量分支 | ✅ |
| T-Var (类型变量查找) | `synthesize_flat` Variable 分支 | ✅ |
| T-App (函数应用推断) | `synthesize_flat` Call 分支 | ✅ |
| T-Lambda (函数类型合成) | `synthesize_flat` Lambda 分支 | ✅ |
| T-If (条件类型合并) | `synthesize_flat` If 分支 + check-mode | ✅ |
| T-Let / T-LetRec | `synthesize_flat` Let/LetRec 分支 | ✅ |
| T-Begin (序列类型) | `synthesize_flat` Begin 分支 + check-mode | ✅ |
| T-TypeAnnotation (显式类型标注) | TypeAnnotation 分支 + CastOp emit | ✅ |
| T-BiDirectional check-mode | If/Let/Begin/Annotation/Set/Define | ✅ |
| T-ValueRestriction | `syntactic_value` 检查 | ✅ |

### 2.2 渐进类型

| 机制 | 状态 | 说明 |
|------|------|------|
| 动态类型 `?` (Dynamic) | ✅ | TypeTag::Dynamic 作为底类型 |
| 一致性关系 `T ~ U` | ✅ | `consistent_unify`: 允许 ? 与任意类型匹配 |
| 类型转换 (Coercion) | ✅ | `is_coercible` + `add_coercion` 在类型不一致时插入 CastOp |
| 调用点 coercion | ✅ | lowering 层参数类型检查 + CastOp 插入 |
| 运行时 CastOp | ✅ | IR/JIT CastOp, 扩展 type_tag 覆盖 Int/Float/Bool/String |

### 2.3 Occurrence Typing

| 特性 | 状态 | 说明 |
|------|------|------|
| 谓词分析 | ✅ | `analyze_predicate_flat`: `number?`/`string?`/`pair?`/`symbol?`/`float?`/`boolean?` |
| 类型窄化 | ✅ | `if` 分支中根据谓词结果窄化变量类型 |
| `type?` 原语 | ✅ | 运行时类型查询 |
| and/or 组合 | ✅ | 布尔谓词组合窄化 |

### 2.4 约束求解

| 机制 | 状态 |
|------|------|
| EQUAL 约束 (完全一致) | ✅ |
| CONSISTENT 约束 (渐进一致) | ✅ |
| Union-Find 求解 | ✅ T2a — multi-pass worklist fixpoint |
| 函数参数/返回分解 | ✅ |
| Let-Poly (let多态) | ✅ T4 | `is_poly` 由 `forall_of` 决定，lookup 调用 `instantiate_forall` |

### 2.5 ADT 类型推断

| 特性 | 状态 | 说明 |
|------|------|------|
| `define-type` 类型注册 | ✅ T2b | 构造函数 + 字段类型签名 |
| `match` 模式类型检查 | ✅ T2b | 构造函数模式 + 字段绑定 |
| 多态构造函数 | ✅ T2b | forall-wrapped 类型变量 |
| 参数化 ADT | ✅ T2b | `(Option Int)` 等 |
| 穷尽性检查 | ✅ T4 | `__match_tmp` + `get_adt_constructors` 对比，wildcard 跳过 |

### 2.6 Blame 结构化

| 机制 | 状态 |
|------|------|
| BlameParty 枚举 | ✅ T2c — 调用方/定义方 |
| BlameInfo 结构 | ✅ T2c — blame 标签 + 位置信息 |
| Consistent-subtype | ✅ T2c |
| Coercion marker pass | ✅ T2c — 标记需运行时检查的 coercion |

### 2.7 Value Restriction

| 规则 | 状态 | 说明 |
|------|------|------|
| `syntactic_value` 检查 | ✅ | 非值表达式（调用、mutation）的 let 绑定不泛化 |
| 禁止非值 let 泛化 | ✅ | 防止类型不安全 |

### 2.8 DeadCoercionEliminationPass

| 规则 | 状态 | 说明 |
|------|------|------|
| 冗余 CastOp 消除 | ✅ T2e | IR 级 pass，消除类型相同的 CastOp |
| Gradual Guarantee 测试 | ✅ T2e | 10+ 测试用例验证类型替换体面 |

## 3. 与 IR 的解耦

### 3.1 解耦程度（中 — 通过 CastOp 耦合）

```
┌───────────────────┐     ┌──────────────────────┐
│    TypeChecker     │     │   IR Pipeline         │
│  (FlatAST 层面)    │     │  (IRModule + passes)  │
│                   │     │                      │
│  infer_flat()      │     │  lower_to_ir()       │
│  → TypeId         │     │  → IRInstruction[]   │
│  → Diagnostic[]   │     │  → CastOp (带 type_tag)│
│  → CastOp 插入     │     │                      │
│                   │     │  DeadCoercionEliminate│
└───────────────────┘     └──────────────────────┘
```

**改进后的耦合点**：
- `IRInstruction` 有 `opcode + operands[4]`，CastOp 携带 type_tag
- `lowering_impl.cpp` 在 TypeAnnotation 边界 + call-site 参数处读 type_id 并插入 CastOp
- `DeadCoercionEliminationPass` 消除冗余 CastOp

## 4. 与 EDSL / Typed Mutation 的关系

```
set-code → query → typed-mutate → eval-current → TypeChecker
                                                → query:type
```

- `query:type` 原语 — 查询 AST 节点的推断类型（`TypeResolutionIndex`, M2.7）
- `eval-current` 通过标准 `eval()` 路径触发 TypeChecker
- EDSL 操作本身 (`query:*`, `mutate:*`) 全部走 tree-walker fallback

## 5. 限制与未实现

### 5.1 增量类型检查（实装于 synthesize_flat）

- ✅ `FlatAST::dirty_` 字段存在并被 `synthesize_flat` 读取
- ✅ `if (!flat.is_dirty(id))` 跳过路径已实装
  (`type_checker_impl.cpp:1336`)
- ✅ 缓存的 `type_id` 在脏路径命中时返回；
  缓存有效但需重检查时走 `stale_cache` 计数
- ✅ 统计字段 `cache_hits` / `cache_misses` /
  `stale_cache` 已从 EngineStats 聚合到 TypeChecker
  整体统计

**剩余限制**：脏传播仍按单次 mutation 触发；
多 mutation 串行场景下，可进一步粒化（TODO 跟进）。

### 5.2 类型信息不流向 IR

- `infer_flat()` 返回的 `TypeId` 在 `eval()` 里被忽略（只收集 diagnostics）
- lowering 时无法访问完整的推断类型（仅 TypeAnnotation 边界 + call-site 有 CastOp）
- 无法做基于类型的 IR 优化

### 5.3 Let-Poly（已实装于 TypeEnv::bind + lookup）

- ✅ `Binding::is_poly` 由 `reg_.forall_of(type)` 动态决定
  (`type_checker_impl.cpp:101`)
- ✅ `lookup` 检测 `is_poly` 并调用
  `reg_.instantiate_forall(...)` 泛化
  (`type_checker_impl.cpp:114-115`)
- ✅ `infer_flat` 在 `let` / `letrec` 绑定中
  正确传播 `is_poly` 标志

### 5.4 模块类型签名

- import/require 绑定的类型大部分是 Dynamic
- 无类型签名传播机制

### 5.5 ADT 穷尽性检查（部分实装）

- ✅ `__match_tmp` + `get_match_info` 识别 match 表达式
  (`type_checker_impl.cpp:2318-2319`)
- ✅ `reg_.get_adt_constructors(subject_type)` 对比分支
  (`type_checker_impl.cpp:2333`)
- ✅ 缺失分支以诊断上报，wildcard 跳过

**剩余限制**：当前仅检查
`__match_tmp` 单层 match；嵌套 match
在外部 match 亪面后不再独立检查。

## 6. 未来改进路径

### Level 2: 增量类型检查（已实装，见 5.1）

- 原始改进路径：利用 `FlatAST::dirty_` 做脏路径跳过
- 缓存类型推断结果到 `type_id_`
- 当前状态：实装于 `synthesize_flat`，含
  `cache_hits` / `cache_misses` / `stale_cache` 统计
- 进一步：粒化到单节点多 mutation 场景

### Level 3: 类型信息进入 IR

- IRInstruction 增加可选 TypeId 字段
- Lowering 时附加上推断的类型
- IRInterpreter 做运行时类型断言（渐进类型）
- LLVM JIT 利用类型生成更优机器码

### Level 4: Let-Poly（已实装，见 5.3）

- 原始改进路径：启用 `TypeScheme::is_poly` 做 let 泛化
- 当前状态：实装于 `TypeEnv::bind` + `lookup`，
  `is_poly` 由 `forall_of` 动态决定
- 进一步：多 mutation 下的 poly 缓存失效

### Level 5: ADT 穷尽性检查（部分实装，见 5.5）

- 原始改进路径：match 模式未覆盖分支警告
- 当前状态：`__match_tmp` + `get_adt_constructors`
  单层检查已实装
- 进一步：嵌套 match 独立检查

## 7. 参考文档

- `design/notes/aura_typesystem_formal.md` — 形式类型规则（历史参考）
- `docs/design/core/typed_mutation.md` — 带类型检查的 EDSL 变异
- `docs/design/compilation/ir_pipeline.md` — IR 管线设计
- `docs/roadmap.md` — 路线图
- `src/compiler/type_checker_impl.cpp` — 类型检查器实现

## 8. Implementation vs Documentation 同步说明

本节记录了文档与实现的最近同步情况（Issue #129）。

### 8.1 同步项

| 项目 | 文档原状 | 实际实现 | 同步后 |
|------|----------|----------|--------|
| 增量类型检查 | 标 "未实装"、每次全量检查 | `synthesize_flat` 已读 `is_dirty` 并返回缓存 `type_id` | 已移入 5.1，标 "实装" |
| Let-Poly | 标 "未完整使用" | `TypeEnv::bind` + `lookup` 联动 `is_poly` + `instantiate_forall` | 已移入 5.3，标 "实装" |
| ADT 穷尽性 | 标 "尚未实现" | `__match_tmp` + `get_adt_constructors` 单层检查 | 已移入 5.5，标 "部分实装" |

### 8.2 同步检查清单

提交 PR 之前应验证：
- 状态表 (2.4 / 2.5) 与 5.x 节描述一致
- 5.x 节 "剩余限制" 描述具体、未夸大
- "未来改进路径" (6) 中已实装项目标注 "已实装"
- 新代码含增量缓存、poly、穷尽性等语义时，同步更新本节
