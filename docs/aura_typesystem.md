# Aura 类型系统设计（代码验证修正版）

**版本**：v0.4
**日期**：2026-05-14
**核心原则**：Sound Gradual Typing × Homoiconic × Queryable + Reflective（规划中）

> ⚠️ **关于本文**：本文档经过代码库全面审计，每条陈述都标注了实现状态。
> - ✅ = 代码已实现并测试通过
> - ⚡ = 部分实现或存在但未完整
> - 📋 = 设计文档/规划中，未实现
> - ❌ = 不存在，纯臆想

---

## 1. 设计哲学

### 1.1 Sound Gradual Typing ✅

```
动态代码  ──边界──▶  静态代码
默认      渐进标注    编译期保证
```

- ✅ 所有代码默认动态：AI 快速原型不需要任何类型标注
- ✅ 通过 `: Type` 注解或推断逐步加强约束
- ✅ `is_coercible` + `consistent_unify` 实现渐近类型一致性检查
- ✅ 类型是 AST/IR 的一等数据（`TypeId` 在 `FlatAST::type_id_` 列中）

### 1.2 Homoiconic 类型语言 ✅

```scheme
(: x Int)
(: foo (-> Int Int))
```

- ✅ 类型标注作为 `Annotation` 节点嵌入 FlatAST
- ✅ 类型可通过 type-of 运行时查询
- 📋 P2996 编译期验证 — **未实现**（`type_info.ixx` 为空文件）
- ✅ 运行时内省：`type-of` 原语、`type?` 谓词已实现

### 1.3 Reflective ⚡

- ⚡ 运行时闭包内省：`--inspect` 显示闭包的 `param_types`、`func_free_vars` 等
- ❌ P2996 编译期验证 — 代码中无任何 `consteval` 反射使用
- ❌ 宏中类型验证 — 不存在

### 1.4 Queryable ✅

- ✅ `QueryExpr::Kind::HasType`、`ReturnType`、`ArgType` three type-based query clauses 已实现
- ❌ 文档中 `no-annotation?`、`any-arg-has-type?`、`usage-count` 等高级查询 clause **不存在**

---

## 2. 对标分析

### OCaml

| 特性 | OCaml | Aura 实际情况 |
|------|-------|---------------|
| 推断 | HM 完全推断 | ✅ Bi-directional + HM 核心 |
| ADT | variant/record | ❌ **未实现**。TypeRegistry 仅有 6 个基础类型，无 Tuple/Variant/Record |
| 模块 | functor | 📋 规划中（`docs/module_system_abf_v2.md`）|
| 多态 | 参数多态 | ⚡ `register_forall` 方法存在，**但从未被任何代码调用，0 测试覆盖** |
| 渐进性 | 无 | ✅ Sound gradual（L6.6 已落地）|
| 反射 | Obj 模块（弱） | ❌ **P2996 集成未实现**，仅有空 `type_info.ixx` |
| 查询 | 无 | ✅ 部分实现（3 个 type clause）|

### Typed Racket

| 特性 | TR | Aura 实际情况 |
|------|-----|---------------|
| Soundness | contracts + blame | ✅ `is_coercible` + blame 位置 |
| Occurrence typing | (if (number? x) ...) | ✅ 同支持 |
| Performance | 10,000% overhead | 📋 未 benchmark |
| 宏卫生 | 完整 | 📋 设计阶段（`docs/hygienic_macros.md`）|
| C++ 后端 | 无 | ✅ SoA FlatAST + Arena |

---

## 3. 类型语言（S-表达式语法）

### 3.1 基础类型

```scheme
Int         ;; ✅ 已实现（TypeId index 1）
Bool        ;; ✅ 已实现（TypeId index 2）
String      ;; ✅ 已实现（TypeId index 3）
Any         ;; ✅ 动态类型（TypeId index 0）
Void        ;; ✅ （TypeId index 4）
Type        ;; ✅ 类型自身的类型（TypeId index 5）
```

**以下 TypeTag 在枚举中已声明，但 TypeRegistry 未注册，无法使用：**
```scheme
Symbol      ;; ❌ TypeTag 存在，TypeRegistry 未注册
Float       ;; ❌ TypeTag 存在，TypeRegistry 未注册
Pair        ;; ❌ TypeTag 存在，TypeRegistry 未注册
Vector      ;; ❌ TypeTag 存在，TypeRegistry 未注册
```

**以下在文档中存在但代码中完全不存在：**
```scheme
(List Int)      ;; ❌ 不存在
(Maybe Int)     ;; ❌ 不存在
(Record ...)    ;; ❌ 不存在
(Tuple ...)     ;; ❌ 不存在
(Variant ...)   ;; ❌ 不存在
```

> **关键事实**：`src/core/type_impl.cpp:9-14` — TypeRegistry 构造函数仅注册了
> 6 个类型（Any/Int/Bool/String/Void/Type）。其余 8 个 TypeTag 枚举值只是预留，
> **不能用于任何实际操作**。

### 3.2 函数类型 ✅

```scheme
(-> Int String)               ;; 已实现：register_func 创建 FuncType
(-> Int Int Bool)             ;; 多参数函数
```

⚠️ `(forall [a] (-> a a))` — `register_forall` 存在但**无使用、无测试、无解析支持**。

### 3.3 类型标注位置 ✅

```scheme
(: x Int)                     ;; ✅ Annotation 节点
(let ([x : Int 42]) ...)      ;; ✅ Let 节点带 :param_type
(lambda ([x : Int]) ...)      ;; ✅ Lambda 节点带 :param_type
```

---

## 4. Phased 实现（Trees That Grow）

### 4.1 FlatAST 实际情况

**文档声称**：
```
FlatAST 扩展：
  type_resolved_ → TypeId (vector)
  type_expected_ → TypeId (vector)
```

**实际代码**（`src/core/ast_flat.ixx:119-121`）：
```cpp
// Type information (L6.5+): type_id per node, 0 = DYNAMIC
std::pmr::vector<std::uint32_t> type_id_;
```

✅ 只有**一列** `type_id_`（uint32），没有 `type_resolved_` 和 `type_expected_` 两列。

`TypeInfo` 结构体（`src/core/type.ixx:47-51`）存在但**仅用于计算，不存入 FlatAST**：
```cpp
export struct TypeInfo {
    TypeId resolved{};      // 经过推断的类型
    TypeId expected{};      // 用户标注的类型（可选）
    bool has_annotation = false;
    bool matches() const { return resolved == expected || !expected.valid(); }
};
```

### 4.2 ABF v2 类型扩展 ❌

- 📋 规划中（`docs/module_system_abf_v2.md`）
- ❌ 未实现

### 4.3 P2996 验证 ❌

- `src/core/type_info.ixx` — **空文件**（仅保留位注释）
- ❌ 无任何 `consteval validate_type_layout()` 实现

---

## 5. 类型检查器（Lowering Pass）

### 5.1 架构 ✅

```
AST → FlatAST → TypeChecker（lowering 链中） → AuraIR
                            ↓
                  运行时 coercion 检查（CastNode 在 evaluator/IR executor 中处理）
```

✅ TypeChecker 是 lowering pass 链中的独立 pass
❌ 不存在独立的 `CoercionInsertionPass` — coercion 检查在 `InferenceEngine` 内联完成
❌ `insert_coercions()` 函数不存在

### 5.2 推断算法 ✅

```text
Synthesize:
  LiteralInt  → Int
  Variable x  → 从环境查找
  (f a)       → infer(f) = (-> A B), check(a, A) → B

Check:
  (lambda ([x : A]) e) : (-> A B)
  (if c e1 e2) : T
```

✅ 完整实现：`synthesize_flat_*` 和 `check_flat_*` 系列方法覆盖了 Call/Lambda/If/Let/Begin/Var/Annotation 等节点

### 5.3 类型环境 ✅

```cpp
class TypeEnv {                      // ✅ src/compiler/type_checker.ixx
    void push_scope();
    void pop_scope();
    void bind(name, type);
    TypeId lookup(name);
};
```

### 5.4 约束求解 ✅

```cpp
class ConstraintSystem {             // ✅
    void add(Constraint c);          // ✅ EQUAL / CONSISTENT
    bool solve();                    // ✅ unification
    bool consistent_unify(t1, t2);   // ✅ 渐进 unification（Any 兼容）
    bool occurs_check(var, ty);      // ✅ occurs check
    TypeId normalize(id);            // ✅ 替换归一化
};
```

### 5.5 Occurrence Typing ✅

```scheme
(if (string? x) 
    (string-append x "!")     ;; ✅ x : String（then 分支）
    x)                        ;; ✅ x : not(String)（else 分支）
```

实现（`src/compiler/type_checker_impl.cpp:259-336`）：
- ✅ `OccurrenceInfoFlat` 结构体
- ✅ `analyze_predicate_flat()` 支持 `string?`/`number?`/`int?`/`bool?`/`type?` 及 `not`/`and`/`or`
- ✅ @ in if/and/or 节点分配 refine 环境

---

## 6. 渐进机制（Sound Gradual）

### 6.1 核心：Nominal + RTTI ✅

✅ `is_coercible(from, to)` — 核心逻辑：
- `from == to` → 恒等
- `from == Any || to == Any` → 一致
- `is_func_type(from) && is_func_type(to)` → 分解检查参数/返回
- 其他 → 基础类型相等检查

### 6.2 Coercion 机制 ✅

```
动态 → 静态边界：一致性检查通过（运行时 CastNode）
静态 → 静态边界：恒等
动态 → 动态边界：恒等
```

```cpp
// src/core/ast_flat.ixx:271
NodeId add_coercion(NodeId inner, std::uint32_t type_id);
```

✅ `FlatAST::add_coercion()` — 创建 `CastNode` 携带目标 type_id
✅ 运行时：`IrExecutor` 处理 `CastKind` 指令

### 6.3 Blame 分配 ✅

```cpp
// 错误格式
"calling argument 0: coercion from Int to String at 1:16"
"call return type: coercion from Int to String at 1:1"
```

✅ 每个 coercion 报错包含源位置（`${loc}` 列号行号）
❌ **结构化 blame（JSON blame tree）** — 未实现
❌ **AI auto-fix 格式** — 未实现

---

## 7. FlatAST SoA 层类型

### 7.1 TypeId 系统 ✅

```cpp
// ✅ src/core/type.ixx
enum class TypeTag : uint8_t {    // 14 个枚举值
    DYNAMIC, INT, BOOL, STRING, FLOAT, PAIR, VECTOR,
    CLOSURE, FUNC, RECORD, VARIANT, TYPE_VAR, FORALL, VOID, TYPE
};

struct TypeId {                    // ✅ index + generation
    uint32_t index;
    uint32_t generation;
};
```

### 7.2 TypeRegistry ✅

```cpp
// ✅ src/core/type_impl.cpp
TypeRegistry::TypeRegistry() {
    register_type(TypeTag::DYNAMIC, "Any");   // idx=0
    register_type(TypeTag::INT, "Int");        // idx=1
    register_type(TypeTag::BOOL, "Bool");      // idx=2
    register_type(TypeTag::STRING, "String");  // idx=3
    register_type(TypeTag::VOID, "Void");      // idx=4
    register_type(TypeTag::TYPE, "Type");      // idx=5
}
```

⚠️ `register_func` / `register_forall` 方法存在但仅被 `init_primitive_env()` 的 ~53 个 primitive 调用。

### 7.3 类型优化 Pass ❌

```
Pass 1: 常量折叠               ✅ 已实现（compute_kind + 常量折叠）
Pass 2: 类型特化               ❌ 不存在
Pass 3: 运行时代码提升         ❌ 不存在
Pass 4: Dead cast removal      ❌ 不存在
```

---

## 8. 反射集成

### 8.1 P2996 验证 ❌

- `src/core/type_info.ixx` — 空文件
- ❌ 无 `consteval validate_type_layout()`、无 `std::meta::members_of` 调用
- ❌ 工具 `aura-reflect` 仅验证 IR instruction 和 schema 布局，不验证类型

### 8.2 运行时类型自省 ✅

```scheme
(type-of 42)      ;; ✅ → Int
(type? 42 "Int")  ;; ✅ → 1 (#t)
(type-of cons)    ;; ✅ → Pair
```

`--inspect` 显示闭包类型 ✅

### 8.3 宏中类型验证 ❌

- ❌ 不存在
- 📋 等待卫生宏实现

---

## 9. AI 操作集成

### 9.1 Query 类型模式 ✅（基础版）

已实现：
```scheme
(query (node-type Call) (has-type? Int))       ;; ✅
(query (node-type Call) (return-type? String)) ;; ✅
(query (argument-type? "Int") (callee "+") )     ;; ✅
```

⚠️ **不支持的高级 clause**（文档有但代码无）：
- `any-arg-has-type?`
- `no-annotation?`
- `usage-count`
- `argument-type 0 String`（文档中带索引的写法，代码只支持统一语法）

### 9.2 TransformEngine 类型修复 ❌

- ❌ 不存在类型感知的 transform
- ❌ `auto-fix` 仅做代码结构变换，无类型推导的修复

### 9.3 AI 工作流 ⚡

工作流中**第 5 步（Reflection → P2996）** 和**第 6 步（类型优化 Pass）** 未实现。
第 7 步（运行时 blame）部分存在（行号位置，无结构化 blame tree）。

---

## 10. 实际实现路线图（对比原文档 ✔/✘）

### L6.1 TypeId 骨架 ✅
- [x] TypeTag enum（14 个值）— `src/core/type.ixx`
- [x] TypeRegistry（6 个注册类型）— `src/core/type_impl.cpp`
- [x] TypeInfo struct — `src/core/type.ixx`

### L6.2 解析器扩展 ✅
- [x] `(: x Int)` 标注解析 — `flat_parser_impl.cpp`
- [x] type-of / type? 运行时原语

### L6.3 TypeChecker ✅
- [x] `InferenceEngine` + `TypeEnv` + `ConstraintSystem` — `src/compiler/type_checker_impl.cpp`（676行）
- [x] ~53 primitive 类型签名 — `init_primitive_env()`

### L6.4 推断 ✅
- [x] Bi-directional synthesize/check（Call/Lambda/If/Let/Begin/Var/Annotation）
- [x] `consistent_unify` + `occurs_check` + `normalize`

### L6.5 Query 类型扩展 ✅
- [x] `HasType` / `ReturnType` / `ArgType` 三种 type clause

### L6.6 渐进机制 ✅
- [x] `is_coercible` — Any ↔ T 一致性
- [x] `add_coercion` → CastNode
- [x] 运行时 coercion 检查

### L6.7 Occurrence Typing ✅
- [x] 分析 `string?`/`number?`/`int?`/`bool?`/`type?`
- [x] `not`/`and`/`or` 组合谓词
- [x] if 分支类型细化

### 未实现/部分特性（原文档大量篇幅，但代码未落地的）

| 特性 | 状态 | 文档位置 |
|------|------|----------|
| Tuple/Variant/Record 类型语言 | ❌ 不存在 | §3.2 复合类型 |
| (List Int) / (Maybe Int) 语法糖 | ❌ 不存在 | §3.2 |
| forall 多态 + let-polymorphism | ❌ 代码未使用 | §6.1, §13.4 |
| P2996 编译期类型验证 | ❌ 空文件 | §8.1 |
| CoercionInsertionPass（独立 pass） | ❌ 内联实现 | §14.3 |
| 类型特化/Dead cast removal Pass | ❌ 不存在 | §7.3 |
| 结构化 blame（JSON blame tree） | ❌ 字符串报错 | §6.3 |
| 线性所有权 (Linear T / move / borrow) | ❌ 不存在 | §20 |
| ABF v2 类型扩展 | ❌ 未实现 | §4.2 |
| 依赖/细化类型 | ❌ 不存在 | §11 |
| `no-annotation?`/`any-arg-has-type?` query clauses | ❌ 不存在 | §9.1 |
| 宏中类型验证 | ❌ 不存在 | §8.3 |
| 子类型系统 | ❌ 不存在 | §11 |
| 形式类型规则（附录 formal 文档） | 📋 只有 `T-Int/T-Bool` 等基础规则对应代码 | §13 |

---

## 11. 差距分析与优先级建议

### 高优先级（填补已有地基）

1. **补齐 TypeRegistry 注册** — `Symbol`/`Float`/`Pair`/`Vector` 的 TypeTag 值已声明但未注册，导致这些"类型"完全不可用。5 分钟改动。

2. **forall 测试覆盖** — `register_forall` 已实现但 0 调用。加 3-5 个测试就能启用。

3. **结构化 blame** — 当前只有字符串报错。返回 JSON blame tree 对 AI auto-fix 至关重要（约 1 天）。

### 中优先级（文档超前代码太多）

4. **修正类型语言文档与代码的差距** — 删除不存在的复合类型语法举例，或者实现它们。

5. **TypeInfo 存入 FlatAST** — 代码中 `TypeInfo` 仅在 pass 内存在，不持久化。考虑是否需要两列（resolved/expected）还是单列已够用。

### 低优先级（M4/M5 路线）

6. P2996 反射集成
7. 类型优化 Pass
8. 线性所有权
9. 多态深化

---

## 12. 红线清单（当前实现覆盖）

### ✅ 已通过
- `(: x Int)` 推断 Int
- `(+ 1 2)` 推断 Int
- `(string-append "a" "b")` 推断 String
- `(type-of 42)` → Type
- `(type? 42 "Int")` → Bool
- occurrence: `(if (string? x) x "fallback")` → String
- coercion: `(+ "42" 1)` → Int（运行时 cast）

### ❌ 未测试/不可用
- `(cast 42 : Int)` — CastNode 类型检查未测试
- `(query (has-type? Int))` — 需要更多集成测试
- `forall` 多态代码 — 解析器不支持 `(forall [a] ...)` 语法

---

## 13. 结论

**核心类型系统（L6.1-L6.7）是健康的**，约 1700 行代码覆盖了 Sound Gradual Typing + Occurrence Typing + 查询的核心功能。

**但设计文档严重超前于代码**，§3 的类型语言、§13 的形式规则、§14 的算法、§20 的线性所有权，大部分是愿景而非已落地的工程。

建议下一步：
1. 修正类型语言文档（今天就做）
2. 注册 Symbol/Float/Pair 类型使其可用（30 分钟）
3. 加 forall 测试验证 register_forall 是否工作（1 小时）
4. 结构化 blame（1 天）— 对 AI 循环最有价值
