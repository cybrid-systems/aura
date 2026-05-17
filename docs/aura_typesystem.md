# Aura 类型系统

**状态**: 渐进类型 L6，集成到 IR 管线（warnings-only 模式）
**源码**: `src/compiler/type_checker.ixx`, `src/core/type.ixx`, `src/compiler/diag.ixx`

## 1. 架构总览

```
TypeChecker (对外 API)
 ├── infer_flat(FlatAST&, StringPool&, NodeId, DiagnosticCollector&) → TypeId
 └── check_flat(FlatAST&, StringPool&, NodeId, TypeId expected)
       │
       ▼
InferenceEngine
 ├── synthesize_flat(...)  — 类型合成（自下而上）
 ├── check_flat(...)    — 类型检查（给定预期类型）
 └── analyze_predicate_flat(...) — Occurrence Typing 谓词分析
       │
       ▼
TypeEnv       — 作用域 + 类型绑定（支持 let-polymorphism 骨架）
ConstraintSystem — unification 求解：EQUAL / CONSISTENT
TypeRegistry — 核心类型定义存储（持久）
```

### 执行管线中的位置

```
eval(expr):
  parse → macro_expand → needs_fallback?
    ├── yes → eval_flat (tree-walker, 无类型检查)
    └── no  → TypeChecker.infer_flat()    ← 类型检查（warnings only）
               ↓
              lower_to_ir_with_cache()     ← 类型信息不向下传
               ↓
              passes → IRInterpreter       ← 弱类型执行
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
| T-If (条件类型合并) | `synthesize_flat` If 分支 | ✅ |
| T-Let / T-LetRec | `synthesize_flat` Let/LetRec 分支 | ✅ |
| T-Begin (序列类型) | `synthesize_flat` Begin 分支 | ✅ |
| T-TypeAnnotation (显式类型标注) | TypeAnnotation 分支 | ✅ |

### 2.2 渐进类型

| 机制 | 状态 | 说明 |
|------|------|------|
| 动态类型 `?` (Unknown) | ✅ | TypeTag::Dynamic 作为底类型 |
| 一致性关系 `T ~ U` | ✅ | `consistent_unify`: 允许 ? 与任意类型匹配 |
| 类型转换 (Coercion) | ✅ | `add_coercion` 在类型不一致时插入 CastNode |
| 运行时 CastOp | ⚡ | IR 的 CastOp 硬编码 `type_tag=3(dynamic)` |

### 2.3 Occurrence Typing

| 特性 | 状态 | 说明 |
|------|------|------|
| 谓词分析 | ✅ | `analyze_predicate_flat`: `number?`/`string?`/`pair?` 等 |
| 类型窄化 | ✅ | `if` 分支中根据谓词结果窄化变量类型 |
| `type?` 原语 | ✅ | 运行时类型查询 |

### 2.4 约束求解

| 机制 | 状态 |
|------|------|
| EQUAL 约束 (完全一致) | ✅ |
| CONSISTENT 约束 (渐进一致) | ✅ |
| 函数子类型分解 | ⚡ 仅 func 参数/返回分解 |
| Let-Poly (let多态) | ❌ `is_poly` 字段未使用 |

## 3. 与 IR 的解耦

### 3.1 解耦程度（高）

```
┌───────────────────┐     ┌──────────────────────┐
│    TypeChecker     │     │   IR Pipeline         │
│  (FlatAST 层面)    │     │  (IRModule + passes)  │
│                   │     │                      │
│  infer_flat()      │     │  lower_to_ir()       │
│  → TypeId         │     │  → IRInstruction[]   │
│  → Diagnostic[]   │     │  → 无 TypeId 字段     │
│                   │     │                      │
│  类型信息在此终止   │     │  不接收类型信息       │
└───────────────────┘     └──────────────────────┘
```

**证据**：
- `IRInstruction` 只有 `opcode + operands[4]`，无 TypeId
- `lowering_impl.cpp` 不读 `FlatAST::type_id_`
- `IRInterpreter` 对类型完全无感知
- 唯一的 `CastOp` 硬编码 `type_tag=3`（dynamic）

### 3.2 仅有的耦合点

| 耦合点 | 说明 |
|--------|------|
| 管线顺序 | 类型检查必须在 lowering 之前 |
| `type_registry_` | CompilerService 持久的类型定义注册表 |
| `cache_define()` | 函数缓存前先类型检查 |
| `CastOp` | IR 唯一类型感知 opcode（但信息流是断的） |

## 4. 与 EDSL / Typed Mutation 的关系

```
set-code → query → typed-mutate → eval-current
  ↑                    ↓
  类型检查只在 eval-current 时运行
  (通过 eval() 中的 TypeChecker pass)
  typed-mutate 本身不触发类型检查
```

- `typed-mutate` 原语（`typed_mutation_design.md`）在修改 AST 节点时验证类型兼容性
- `eval-current` 通过标准 `eval()` 路径触发 TypeChecker
- EDSL 操作本身 (`query:*`, `mutate:*`) 全部走 tree-walker fallback

## 5. 限制与未实现

### 5.1 增量类型检查（未实装）

Roadmap 提到 "dirty skip + type cache"，但当前：
- `FlatAST::dirty_` 字段存在（用于 EDSL mutate 的增量编译）
- TypeChecker 不读 `dirty_`，每次全量检查
- 类型推断结果不缓存到 `type_id_`

### 5.2 类型信息不流向 IR

- `infer_flat()` 返回的 `TypeId` 在 `eval()` 里被忽略（只收集 diagnostics）
- lowering 时无法访问推断出的类型
- 无法做基于类型的 IR 优化

### 5.3 Let-Poly 未使用

- `TypeScheme::is_poly` 字段存在但始终为 false
- `let` 绑定不做泛化
- 多态函数类型退化为单一实例化

### 5.4 诊断策略

- 当前所有类型错误都是 warnings（`std::println(stderr, "type warning: ...")`）
- 无 "--strict-typecheck" 模式
- 无类型错误中断执行机制

## 6. 未来改进路径

### Level 2: TypeCheckPass（正式 Pass）

把类型检查做成 `PassManager` 的一员：

```cpp
struct TypeCheckWrap {
    std::string name() const { return "TypeCheck"; }
    void run(IRModule& mod);  // 当前: 无操作
    // 或：在 lowering 前对 FlatAST 做检查
    TypeId check_before_lowering(FlatAST&, StringPool&, NodeId, DiagnosticCollector&);
};
```

### Level 3: 类型信息进入 IR

- IRInstruction 增加可选 TypeId 字段
- Lowering 时附加上推断的类型
- IRInterpreter 做运行时类型断言（渐进类型）
- LLVM JIT 利用类型生成更优机器码

## 7. 与工业界的对比

| 引擎 | 类型与 IR 的关系 | Aura 对比 |
|------|-----------------|-----------|
| JVM | 字节码带类型，JIT 高度依赖 | 解耦度更高 |
| V8 Ignition | 字节码无类型，TurboFan 靠反馈 | 最接近 |
| Python | 无静态类型，运行时检查 | 类似 |
| **Aura** | **AST 层类型检查，IR 无类型** | **解耦良好，偏动态** |

## 8. 参考文档

- `docs/aura_typesystem_formal.md` — 形式类型规则（设计参考，大部分未落地）
- `docs/typed_mutation_design.md` — 带类型检查的 EDSL 变异
- `docs/ir_pipeline_design.md` — IR 管线设计（含类型检查集成点）
- `docs/roadmap.md` — 路线图
