# Aura 类型系统

**状态**: 渐进类型 L6 + T2a-T2e 完整管线，集成到 IR 管线（warnings-only 模式）
**源码**: `src/compiler/type_checker_impl.cpp`, `src/core/type_impl.cpp`, `src/compiler/diag.ixx`

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
| Let-Poly (let多态) | ⚡ `is_poly` 字段存在但未完整使用 |

### 2.5 ADT 类型推断

| 特性 | 状态 | 说明 |
|------|------|------|
| `define-type` 类型注册 | ✅ T2b | 构造函数 + 字段类型签名 |
| `match` 模式类型检查 | ✅ T2b | 构造函数模式 + 字段绑定 |
| 多态构造函数 | ✅ T2b | forall-wrapped 类型变量 |
| 参数化 ADT | ✅ T2b | `(Option Int)` 等 |
| 穷尽性检查 | 🟡 | 尚未实现 |

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

### 5.1 增量类型检查（未实装）

- `FlatAST::dirty_` 字段存在（用于 EDSL mutate 的增量编译）
- TypeChecker 不读 `dirty_`，每次全量检查
- 类型推断结果不缓存到 `type_id_`

### 5.2 类型信息不流向 IR

- `infer_flat()` 返回的 `TypeId` 在 `eval()` 里被忽略（只收集 diagnostics）
- lowering 时无法访问完整的推断类型（仅 TypeAnnotation 边界 + call-site 有 CastOp）
- 无法做基于类型的 IR 优化

### 5.3 Let-Poly 未使用

- `TypeScheme::is_poly` 字段存在但始终为 false
- `let` 绑定不做泛化
- 多态函数类型退化为单一实例化

### 5.4 模块类型签名

- import/require 绑定的类型大部分是 Dynamic
- 无类型签名传播机制

### 5.5 ADT 穷尽性检查

- `match` 模式检查不会警告未覆盖的分支

## 6. 未来改进路径

### Level 2: 增量类型检查

- 利用 `FlatAST::dirty_` 做脏路径跳过
- 缓存类型推断结果到 `type_id_`

### Level 3: 类型信息进入 IR

- IRInstruction 增加可选 TypeId 字段
- Lowering 时附加上推断的类型
- IRInterpreter 做运行时类型断言（渐进类型）
- LLVM JIT 利用类型生成更优机器码

### Level 4: Let-Poly 启用

- 启用 `TypeScheme::is_poly` 做 let 泛化
- 约束求解器支持类型方案实例化

## 7. 参考文档

- `docs/aura_typesystem_formal.md` — 形式类型规则（设计参考）
- `docs/typed_mutation_design.md` — 带类型检查的 EDSL 变异
- `docs/ir_pipeline_design.md` — IR 管线设计
- `docs/roadmap.md` — 路线图
- `src/compiler/type_checker_impl.cpp` — 类型检查器实现
