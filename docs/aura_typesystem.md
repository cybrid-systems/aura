# Aura 类型系统设计

**版本**：v0.3 （草案）
**日期**：2026-05-12
**状态**：设计+实现阶段 — L6.1 TypeId 骨架已启动
**核心原则**：Sound Gradual Typing × Homoiconic × Reflective × Queryable — 为 AI 而生

---

## 1. 设计哲学

Aura 类型系统的设计由四个核心理念驱动：

### 1.1 Sound Gradual Typing（来自架构文档）

```
动态代码  ──边界──▶  静态代码
默认      渐进标注    编译期保证
```

- 所有代码默认动态：AI 快速原型、REPL 探索不需要任何类型标注
- 通过 `: Type` 注解或 Query 推断逐步加强约束
- Typed 区域的 soundness：无类型错误通过类型有不同 meaning 的边界泄漏
- **关键规则**：类型是 AST/IR 的一等数据，不是编译期移除的装饰

### 1.2 Homoiconic 类型语言

```scheme
(: x Int)
(: foo (-> Int Int))
(: bar (forall [a] (-> a a)))
```

类型本身就是 S-表达式，在 Aura 中作为 `QuoteNode` 存储，可由：
- AI 查询（`(query (node-type annotation) (type Int))`）
- P2996 编译期验证
- 宏系统操作/生成
- 运行时内省

### 1.3 Reflective（M3 核心）

类型系统与 P2996 反射深度绑定：
- 编译期：`std::meta` 验证 AI 生成的类型标注
- 运行时：闭包内省（`param_types`、`return_type`）
- 宏：`(syntax-parse expr : Int)` 在展开时验证

### 1.4 Queryable（M2 核心）

```scheme
;; 查询所有未标注类型的 Call 节点
(query (node-type Call) (no-annotation? #t))

;; 查找类型冲突：(+ str x) 中 str 为string
(query (node-type Call) (operator +) (argument-type 0 String))

;; 自动修复：插入 cast
(query-and-fix
  (match (Call + (Variable "x") _))
  (replace (cast x Int)))
```

---

## 2. 对标分析

### OCaml

| 特性 | OCaml | Aura 借鉴方案 |
|------|-------|---------------|
| 推断 | HM 完全推断 | Bi-directional + HM 核心 |
| ADT | variant/record | `(Variant ...)` / `(Tuple ...)` S-表达式 |
| 模块 | functor | 宏 + 反射（"模块即宏"） |
| 多态 | 参数多态 | `forall` + monomorphization |
| 渐进性 | 无 | **核心差异**：sound gradual |
| 反射 | Obj 模块（弱） | **核心差异**：P2996 + flambda |
| 查询 | 无 | **核心差异**：QueryEngine |

### Typed Racket

| 特性 | TR | Aura 借鉴方案 |
|------|-----|---------------|
| Soundness | contracts + blame | nominal + RTTI + coercions |
| Occurrence typing | (if (number? x) ...) | 同样支持 |
| Performance | 10,000% overhead | ~10% (Muehlboeck/Tate) |
| 宏卫生 | 完整 | 需要相同保证 |
| C++ 后端 | 无 | **核心优势**：SoA + 零开销 |

---

## 3. 类型语言（S-表达式语法）

### 3.1 基础类型

```scheme
Int         ;; 整数
Bool        ;; 布尔
String      ;; 字符串
Symbol      ;; 符号
Float       ;; 浮点数（Phase L4）
Vector      ;; 向量（Phase L5）
Pair        ;; 序对（未标注的）
Any         ;; 动态类型（默认）
Void        ;; 无返回值
```

### 3.2 复合类型

```scheme
;; 函数类型
(-> Int String)                ;; Int → String
(-> Int Int Bool)              ;; Int → Int → Bool（柯里化）
(forall [a] (-> a a))          ;; ∀a. a → a（多态）

;; 积类型（元组）
(Tuple Int String Bool)        ;; (Int, String, Bool)

;; 和类型（变体）
(Variant (Ok Int) (Err String))  ;; Ok Int | Err String

;; 记录
(Record (x Int) (y String))    ;; { x: Int, y: String }

;; 可选
(Maybe Int)                    ;; (Variant (Some Int) None)

;; 列表
(List Int)                     ;; 递归展开为 (Variant (cons (Pair Int (List Int))) nil)
```

### 3.3 类型标注位置

```scheme
;; 变量
(: x Int)
(let ([x : Int 42]) ...)

;; 参数
(lambda ([x : Int] [y : String]) ...)
(-> (Int String) Bool)   ;; 同上，函数类型写法

;; 返回值
(: (-> Int Int))         ;; 函数类型推断
(lambda ([x : Int]) : Int (+ x 1))

;; 结构体字段
(define-struct Point ([x : Int] [y : Float]))
(: Point-x (-> Point Int))
```

### 3.4 类型别名

```scheme
(type-alias Integer Int)
(type-alias StringList (List String))

(-> Integer Integer)
```

---

## 4. Phased 实现（Trees That Grow）

### 4.1 TypedPhase 扩展

每个 AST 节点扩展 `TypeInfo`：

```
ParsedPhase:  无类型信息 (monostate)
TypedPhase:  TypeInfo { resolved_type, expected_type, annotation_pos }
```

```cpp
struct TypeInfo {
    TypeId resolved_type;      // 经过推断的类型
    TypeId expected_type;      // 标注的类型（可选）
    SourceLoc annotation_pos;  // : Type 标注所在位置
};

// Node 扩展示例
struct CallNode {
    // ParsedPhase:
    std::vector<Expr*> args;

    // TypedPhase:
    TypeInfo ret_type;         // 返回类型
    std::vector<TypeInfo> arg_types;  // 参数类型
};
```

### 4.2 序列化（ABF v2 扩展）

```cpp
// 类型信息作为 ABF extension（前向兼容）
struct TypeExtension {
    TagType tag;        // EXT_TYPE_INFO
    uint32_t length;
    TypeId resolved;
    TypeId expected;    // 0 = 无标注
    uint32_t annotation_file_offset;
    uint32_t annotation_line;
};
```

### 4.3 FlatAST SoA 扩展

```
FlatAST:
  tag_         → NodeTag
  int_val_     → int64_t
  sym_id_      → std::string
  arg0_ ...    → NodeHandle

  // 新增类型扩展：
  type_resolved_ → TypeId (vector)
  type_expected_ → TypeId (vector)
```

P2996 反射验证：`validate_type_info()` 编译期检查偏移对齐。

---

## 5. 类型检查器（Lowering Pass）

### 5.1 架构

类型检查作为 lowering pass 链中的独立 Pass：

```
AST → [ParsedPhase] → TypeChecker → [TypedPhase] → Lowering → AuraIR
                          ↓
                  CoercionInsertionPass
                          ↓
                  (cast ...) nodes
```

### 5.2 推断算法：Bi-directional + HM 核心

```text
Synthesize (自上而下):
  LiteralInt  → Int
  LiteralStr  → String
  Variable x  → 从环境查找
  (f a)       → infer(f) = (-> A B), check(a, A) → B

Check (自下而上):
  (lambda ([x : A]) e) : (-> A B)
    注：A 已知，只需推断 e 得 B
  (if c e1 e2) : T
    注：check(e1, T), check(e2, T), check(c, Bool)
```

### 5.3 类型环境

```cpp
struct TypeEnv {
    // 不可变级别（Hyperstatic scope）
    std::span<const TypedBinding> global_types;  // (define x : Int 42)
    std::span<const TypedBinding> local_types;   // (let ([x : Int ...]) ...)

    // Lambda 参数类型
    std::span<const TypeId> param_types;

    // 当前函数返回类型（用于递归 check）
    TypeId return_type;
};
```

### 5.4 约束收集

```cpp
struct TypeConstraint {
    enum Kind { EQUAL, SUBTYPE, INSTANTIATE };
    TypeVar lhs, rhs;
    SourceLoc loc;  // 用于 blame
};

struct TypeChecker {
    TypeEnv env;
    std::vector<TypeConstraint> constraints;
    TypeRegistry& registry;
    DiagnosticCollector& diag;

    TypeInfo synthesize(Expr* e);
    void check(Expr* e, TypeId expected);
    bool solve_constraints();
    ast::Expr* insert_coercions(Expr* typed_ast);
};
```

### 5.5 Occurrence Typing（类 Typed Racket）

```scheme
;; occurrence typing 在 Aura：
(if (string? x) 
    (string-append x "!")   ;; 此处 x 是 String
    x)                       ;; 此处 x 不是 String
```

实现：TypeChecker 在 `(string? x)` 后分路径记录 `x : String` 和 `x : not(String)`。

对 Aura 尤其重要：AI 生成的代码常含类型谓词 + if 分支。

---

## 6. 渐进机制（Sound Gradual）

### 6.1 核心选择：Nominal + RTTI（Muehlboeck/Tate 2017）

Aura 选择 nominal 路径而非纯结构 gradual：

**理由**：
- Aura 已有 SoA + NodeTag + TagIndex — 天然支持 nominal 类型 ID
- C++ 后端可生成高效 RTTI（`dynamic_cast` 级别）
- 最坏边界开销可控 <10%（已由 2017 论文实验证明）
- 每类型单次大对象检查避免分摊爆炸

**代价**：
- 牺牲部分结构子类型（但 Lisp 代码很少依赖结构子类型）
- 通过 `(cast ...)` 显式转换弥补

### 6.2 Coercion 机制

```
动态 → 静态边界：运行时类型检查
静态 → 静态边界：编译期擦除
动态 → 动态边界：无开销
静态 → 动态边界：完全擦除
```

```cpp
// 运行时 Coercion
// (display x) 中 x : Any
// 编译器插入：
struct AnyToInt {
    static int64_t coerce(EvalValue v) {
        if (!std::holds_alternative<int64_t>(v))
            throw TypeError{"expected Int, got " + type_name(v)};
        return std::get<int64_t>(v);
    }
};

// 函数边界 Coercion
// (map (lambda ...) xs) 中 f : (-> Any Any)
// Coercion 在调用点插入
```

### 6.3 Blame 分配

```scheme
;; Aura blame 使用 LocatedPhase 位置信息
;; 错误格式：
TypeError: expected Int, got String
  in: (+ "hello" 1)
  blamed: caller (+ ...)  ← 调用者
  annotation: (: x Int)   ← 约束来源（如有）
  at: source.aura:3:5     ← LocatedPhase 位置
```

结构化错误对 AI auto-fix 至关重要：
```scheme
(query-and-fix
  (match (TypeError (expected Int) (got String) _))
  (replace-type (cast <arg> Int)))
```

---

## 7. IR 层类型（AuraIR）

### 7.1 TypeId 系统

```cpp
enum class TypeTag : uint8_t {
    DYNAMIC = 0,    // Any/无标注
    INT,            // int64_t
    BOOL,           // bool
    STRING,         // string_ref
    FLOAT,          // double
    PAIR,           // (cons ...)
    VECTOR,         // 动态向量
    CLOSURE,        // 闭包函数类型
    FUNC,           // 参数化函数类型
    RECORD,         // 记录类型
    VARIANT,        // 和类型
    TYPE_VAR,       // 类型变量
    FORALL,         // 多态
};

// IR 指令携带类型信息
// 例如 Call 指令：
struct IRCall {
    TypeId return_type;      // 编译期已知的返回类型
    std::vector<TypeId> arg_types;  // 参数类型
};
```

### 7.2 类型 Registry

```cpp
class TypeRegistry {
    // 全局类型注册表
    TypeId register_type(TypeTag tag, std::string name);
    TypeId register_func(std::vector<TypeId> args, TypeId ret);
    TypeId register_forall(TypeVar var, TypeId body);

    // 查询
    TypeTag tag_of(TypeId id);
    std::string_view name_of(TypeId id);
    bool is_subtype(TypeId sub, TypeId sup);
};
```

### 7.3 类型优化 Pass

```
Pass 1: 常量折叠（已有） → 对 LiteralInt 推断类型
Pass 2: 类型特化          → Copy propagate typed values
Pass 3: 运行时代码提升    → 热路径用精确类型
Pass 4: Dead cast removal → 消除冗余 coercions
```

---

## 8. M3 反射集成

### 8.1 P2996 编译期类型验证

```cpp
template<typename T>
consteval bool validate_type_layout() {
    // 用 P2996 检查 TypeInfo 扩展的结构对齐
    auto members = std::meta::members_of(^T);
    // 验证每个字段按预期偏移排列
    return all_fields_valid(members);
}

// AI 代码验证
template<typename Fn>
consteval auto type_of(Fn f) {
    // P2996 反射 AI 函数的类型
    return std::meta::type_of(^f);
}
```

### 8.2 运行时类型自省

```scheme
;; --inspect 扩展
./aura --inspect --type   ;; 显示所有已注册类型
./aura --inspect : foo    ;; 显示 foo 函数类型

;; REPL 查询
> (:type +)
(-> Int Int Int)
> (:type cons)
(forall [a b] (-> a b (Pair a b)))
```

### 8.3 宏中类型验证

```scheme
(defmacro (assert-type x ty)
  `(unless (:instance? ,x ,ty)
     (error "type mismatch: expected " ',ty)))
```

展开时可嵌入 P2996 `static_assert`：

```scheme
(defmacro (ensure-type expr ty)
  ;; 展开时：如果 ty 是 Int
  ;; 插入 static_assert(std::is_same_v<decltype(expr), int64_t>)
  ...)
```

---

## 9. AI 操作集成

### 9.1 Query 类型模式

```scheme
;; 注意核心
(query (node-type Call) (operator type) (any-arg-has-type? String))

;; 安全分析
(query (node-type Call) (op unsafe) (return-type Int))

;; 渐进标注辅助
(query (node-type Variable) (no-annotation? #t) (usage-count > 5))

;; 修复建议
(query-and-fix
  (match (Call (op +) (arg (type String)) (arg (type Int)))
  (fix "consider: " (string-append <arg> (number->string <arg>)))))
```

### 9.2 TransformEngine 类型修复

```scheme
;; 自动插入类型标注
(transform
  (pattern (lambda (x) (+ x 1)))
  (suggest-type :x Int)
  (result (lambda ([x : Int]) (+ x 1))))

;; 类型转换修复
(transform
  (pattern (Call (op +) (arg (type String) . a) (arg (type Int) . b)))
  (fix (Call (op string-append) a (number->string b))))
```

### 9.3 AI 工作流

```
1. AI 生成代码 → 无类型（默认）
2. Query 扫描 → 识别模式（循环、API 调用、递归）
3. 建议类型标注 → TransformEngine 插入
4. TypeChecker running → soundness 验证
5. Reflection 编译期检查 → P2996 校验
6. 编译 → 类型优化 Pass
7. 运行时 → 类型对齐、blame（如果需要）
```

---

## 10. 实现路线图

### Phase L6/7（当前 Sprint）

```
目标：基础类型框架可用
内容：
- TypeId 枚举 + TypeRegistry 骨架
- TypeInfo 结构体（AST 扩展）
- 基础类型 checker（仅 Int/Bool/String）
- S-表达式 类型标注解析
- 与 QueryEngine 的基础集成

红线：
(: x Int)  →  编译通过、运行时验证
(display 42)  →  推断为 Void
(+ "a" 1)  →  编译时警告（混合类型）
```

### M2.7（查询引擎收尾）

```
目标：类型感知查询
内容：
- Query 新增 type 相关 clause
- type-resolution tracking pass
- 基本发生类型（occurrence typing）在 if 后

红线：
(query (node-type Call) (return-type Int))  →  过滤
```

### M3（反射阶段）

```
目标：完整 Sound Gradual
内容：
- Bi-directional checker（完整）
- CoercionInsertionPass
- Blame 分配
- P2996 类型验证扩展
- 运行时闭包类型内省

红线：
(: (-> Int String))  →  sound 验证通过
(cast 42 : Int)      →  编译期 no-op
(cast "hello" : Int) →  运行时 TypeError + blame
```

### M4（生产阶段）

```
目标：性能 + 高级特性
内容：
- Monomorphization（泛型特化）
- Dead coercion elimination pass
- 推断优化（multi-pass solving）
- 模块/签名类型

红线：
已标注代码零运行时开销
混合代码 <10% 开销
```

---

## 11. 风险与开放问题

### 风险

| 风险 | 影响 | 缓解 |
|------|------|------|
| 性能退化（类 Typed Racket） | 高 | 选 nominal 路径 + 论文证明 <10% |
| P2996 与模块系统冲突 | 中 | reflect/ 头文件隔离（已实践） |
| 宏卫生 + 类型传播 | 中 | 参考 TR 的宏类型系统 |
| AI 生成类型错误 | 中 | Occurrence typing + 渐进标注 |

### 开放问题

1. **子类型**：Need nominal subtyping? Lisp 核心几乎不需要
2. **多态**：HM-style let-polymorphism vs 显式 forall
3. **行多态**：记录扩展（类似 OCaml 对象）
4. **依赖类型**：AI 约束验证可能未来需要
5. **外星类型**：C++ FFI 类型映射
6. **Mutability**：set! 的类型效应

---

## 12. 参考

- Muehlboeck & Tate (2017). "Sound Gradual Typing is Nominally Alive and Well"
- Tobin-Hochstadt & Felleisen (2008). "The Design and Implementation of Typed Scheme"
- Pierce (2002). "Types and Programming Languages"
- OCaml manual — type system chapter
- Aura architecture document (§ Sound Gradual Typing)

---

## 13. 形式类型规则（Formal Typing Rules）

> 完整规则见独立附录 [`aura_typesystem_formal.md`](./aura_typesystem_formal.md)。
> 本条为主文档精简摘要。

### 13.1 核心语言语法（子集）

```
e ::= n | b | s | x | (lambda ([x : T]) e) | (e e) | (if e e e)
    | (let ([x : T e]) e) | (cons e e) | (car e) | (cdr e)
    | (string? e) | (number? e) | (pair? e)
    | (cast e : T) | (error T e) | (begin e ...)

T ::= Int | Bool | String | Any | (-> T T) | (Pair T T)
    | (forall [a] T) | a     (类型变量)

Γ ::= · | Γ, x : T           (类型环境，不可覆盖 — Hyperstatic)
     | Γ, a : *              (类型变量环境)
```

### 13.2 语法规则（Typing Rules）

```
----------- T-Int
Γ ⊢ n : Int

----------- T-Bool
Γ ⊢ b : Bool

----------- T-String
Γ ⊢ s : String

Γ(x) = T
----------- T-Var
Γ ⊢ x : T

Γ ⊢ e1 : (-> T1 T2)    Γ ⊢ e2 : T1
------------------------------------ T-App
Γ ⊢ (e1 e2) : T2

Γ, x : T1 ⊢ e : T2
---------------------------- T-Lambda
Γ ⊢ (lambda ([x : T1]) e) : (-> T1 T2)

Γ ⊢ e : Bool    Γ ⊢ e1 : T    Γ ⊢ e2 : T
------------------------------------------ T-If
Γ ⊢ (if e e1 e2) : T

Γ ⊢ e1 : T1    Γ, x : T1 ⊢ e2 : T2
------------------------------------- T-Let
Γ ⊢ (let ([x : T1 e1]) e2) : T2

Γ ⊢ e1 : T1    Γ ⊢ e2 : T2
------------------------------ T-Cons
Γ ⊢ (cons e1 e2) : (Pair T1 T2)

Γ ⊢ e : (Pair T1 T2)
---------------------- T-Car
Γ ⊢ (car e) : T1

Γ ⊢ e : (Pair T1 T2)
---------------------- T-Cdr
Γ ⊢ (cdr e) : T2

Γ ⊢ e : T1    T1 ~ T2
------------------------ T-Cast
Γ ⊢ (cast e : T2) : T2

Γ ⊢ e : String
---------------- T-String?
Γ ⊢ (string? e) : Bool

Γ ⊢ e : T1    T1 ≠ Any    T2 = Type(e的运行时值)
------------------------------------------------------ T-Occurrence
Γ ⊢ (pair? e) : Bool [细化环境：e: not(Any) / e: Pair]
```

### 13.3 渐进规则（Gradual Typing）

```
Any 是最顶层类型：
Γ ⊢ T <: Any    (forall T)
Γ ⊢ Any <: Any

函数子类型（Nominal）：
T1 ~ T1'    T2 ~ T2'
---------------------- T-Fun-Sub
(-> T1 T2) <: (-> T1' T2')     (仅当类型标名相同)

一致性关系（Consistency — 用于混合代码，注意不是相等关系）：
T ~ T                    (自身一致，包含 Any)
T ~ Any                  (动态与任何类型一致)
Any ~ T                  (对称)

关键区别：一致性是 reflexive (总是自反) 但 non-transitive (不传递)。
例如：Int ~ Any ∧ Any ~ Bool ⇒ Int ~ Bool？ 不成立。
一致性仅保证模糊边界处的"可能可转换"，而非"绝对相等"。

Cast 一致性验证：
Γ ⊢ e : T1    T1 ~ T2
-------------------------------- T-Cast-Gradual
Γ ⊢ (cast e : T2) : T2    [运行时检查 T1 ≠ T2 → TypeError]
```

### 13.4 多态规则（Let-Polymorphism）

```
Γ ⊢ e1 : T1    a = ftv(T1) \ ftv(Γ) (泛化)
Γ, x : ∀a. T1 ⊢ e2 : T2
---------------------------------------- T-Let-Poly
Γ ⊢ (let ([x e1]) e2) : T2

Γ(x) = ∀a. T1
Γ ⊢ e2 : [a ↦ T2]T1    (实例化)
---------------------------------------- T-Var-Inst
Γ ⊢ (x e2) : [a ↦ T2]T1'---------------------------------------- T-Var-Inst
Γ ⊢ (x e2) : [a ↦ T2]T1'

--------- T-Let-Poly-Gradual (Value Restriction for Gradual Context)
Γ ⊢ e1 : T1    e1 是语法值 (syntactic value)
a = ftv(T1) \ ftv(Γ)     (无渐近变量参与泛化)
Γ, x : ∀a. T1 ⊢ e2 : T2
----------------------------------------
Γ ⊢ (let ([x e1]) e2) : T2

说明：在渐进语境下，let-polymorphism 仅对语法值启用泛化。
非值 let（如 (let ([x (f 42)]) ...)）退化为 monomorphic 推断。
理由：(cast ...) 在非值位置引入 Any 会污染泛化的类型变量。
这是 ML 的 value restriction 在渐进语境下的自然扩展（参考 Matthesen & Siek）。

特殊规则：对形式为 `(let ([x (cast e : Any)]) ...)` 的非值，x 的类型直接取 Any，
尝试用 `Any` 实例化会退化为运行时检查。```



### 13.5 Soundness（进度/保持定理草稿）

**Progress Theorem**（良类型封闭项要么是值，要么可归约一步）：
```
If ⊢ e : T and e is not a value,
then ∃ e' such that e → e'      (存在归约步骤)
```

Proof sketch: 按类型规则归纳。Any 路径：cast 插入保证运行时检查；cast 本身可归约为检查。

**Preservation Theorem**（归约保持类型）：
```
If ⊢ e : T and e → e',
then ⊢ e' : T                   (类型在归约下不变)
```

Proof sketch: 归约规则保持 RTTI 类型；cast 归约后类型不变（nominal RTTI 保证）。

**Gradual Guarantee**（渐进定理，来自 Siek et al. 2009, 2015）：
```
If Γ ⊢ e : T, and Γ' = replace_annotations(Γ, Any), e' = replace_annotations(e, Any),
then either:
  1) e' evaluates to a value of a type consistent with T', or
  2) e' raises a blame error at a cast boundary.

更精确的静态表述（类型层）：
  If Γ ⊢ e : T, and replace_annotations(T → Any) yields (Γ', e'),
  then Γ' ⊢ e' : Any    (Any 是擦除后的唯一类型)

更精确的动态表述（语义层）：
  If e ⇓ v under Γ, and replace_annotations(Γ ∪ e → Any) yields (Γ', e'),
  then either e' ⇓ v' where consistent(type(v), type(v')) is True,
  or e' ⇓ blame(s) for some blame label s.

关键点：Gradual Guarantee 不保证"不出错"，只保证"不出非 blame 的类型错误"。
这是 sound gradual typing 的核心 —— 动态代码永远不能绕过 typed 区域的类型安全。
```

### 13.6 Occurrence Typing 规则（扩展）

```
Γ ⊢ e : Bool    Γ refine_by (e = #t) ⊢ e1 : T
Γ refine_by (e = #f) ⊢ e2 : T
-------------------------------------- T-If-Occurrence
Γ ⊢ (if e e1 e2) : T

其中 refine_by(pred) 对类型环境进行谓词约束：

```
谓词类型               then 分支细化              else 分支细化
─────────────────────  ──────────────────────    ──────────────────────
(number? e)            e : Int                   e : ¬Int
(string? e)            e : String                e : ¬String
(pair? e)              e : (Pair Any Any)        e : ¬(Pair …)
(boolean? e)           e : Bool                  e : ¬Bool
(null? e)              e : Void                  e : ¬Void
(symbol? e)            e : Symbol                e : ¬Symbol
(float? e)             e : Float                 e : ¬Float
(procedure? e)         e : (-> Any Any)          e : ¬(-> …)
(eq? e1 e2)            e1 : (singleton e2)       (不变)
(and p q)              refine(p) ∩ refine(q)     ¬refine(p) ∪ ¬refine(q)
(or p q)               refine(p) ∪ refine(q)     ¬refine(p) ∩ ¬refine(q)
(not p)                ¬refine(p)                refine(p)
```

**叠加规则**：多个谓词链式叠加。例如：
```scheme
(let ([x : Any 42])
  (if (and (number? x) (> x 0))
      (string-append (number->string x) ">0")     ;; x : Int ∩ {>0}
      x))                                           ;; x : Any(else分支)
```

注意：Occurrence Typing 的细化是单调的（不会撤销），且在函数的入口/出口重置。
```

---

## 14. 约束求解算法

### 14.1 约束收集

```cpp
// TypeChecker::collect_constraints
// 输入：(Expr*, TypeEnv) → 约束表
// 输出：TypeId（推断结果）+ 推导出的约束

enum class ConstraintKind {
    EQUAL,          // T1 == T2（名义相等）
    SUBTYPE,        // T1 <: T2（语法子类型）
    INSTANTIATE,    // ∀a.T → [a↦β]T（用新鲜变量实例化）
    OCCURS,         // a == ... a ... → 递归约束
};

struct Constraint {
    ConstraintKind kind;
    TypeId lhs, rhs;
    SourceLoc loc;  // blame 源
};

class ConstraintCollector {
    TypeEnv& env;
    TypeRegistry& reg;
    std::vector<Constraint> constraints;
    uint64_t fresh_counter = 0;

    TypeId fresh_type_var() {
        return reg.make_var("__t" + std::to_string(fresh_counter++));
    }

    TypeId collect(Expr* e, TypeId expected = TypeId::invalid());
};
```

### 14.2 Unification 算法（Hindley-Milner 核心）

```cpp
class Unifier {
    TypeRegistry& reg;
    std::vector<TypeId> subst;  // 替换映射

    /// consistent_unify —— 渐进 unification
    /// 关键差异 vs 标准 HM unify:
    ///   1. 用 consistent equality (lhs ~ rhs) 替代 lhs == rhs 检查
    ///   2. Any 与任何类型一致（但不绑定变量到 Any — 无信息损失）
    ///   3. 在渐进边界生成 coercion 约束而非类型错误
    bool consistent_unify(TypeId lhs, TypeId rhs, SourceLoc loc) {
        lhs = normalize(lhs);
        rhs = normalize(rhs);

        // consistent equality: Any ~ T always holds
        if (is_any(lhs) || is_any(rhs)) {
            if (is_var(lhs)) return bind_var_unchecked(lhs, rhs);
            if (is_var(rhs)) return bind_var_unchecked(rhs, lhs);
            return true;  // Any ~ concrete: consistent, no binding needed
        }
        
        // 名义相等：lhs == rhs
        if (lhs == rhs) return true;
        
        // 类型变量 → β 替换（standard HM）
        if (is_var(lhs)) return bind_var(lhs, rhs);
        if (is_var(rhs)) return bind_var(rhs, lhs);

        // 函数类型：分解（名义相等——类型标名必须相同）
        if (is_func(lhs) && is_func(rhs)) {
            auto& f_lhs = reg.func_of(lhs);
            auto& f_rhs = reg.func_of(rhs);
            if (f_lhs.args.size() != f_rhs.args.size()) return false;
            for (size_t i = 0; i < f_lhs.args.size(); i++)
                if (!consistent_unify(f_lhs.args[i], f_rhs.args[i], loc))
                    return false;
            return consistent_unify(f_lhs.ret, f_rhs.ret, loc);
        }

        // 基础类型：不相等 → 一致性失败，caller 可决定用 coercion 补救
        return false;
    }

    bool unify(TypeId lhs, TypeId rhs) {
        // 非渐进的 traditional HM unify — 用于 typed-only 区域
        return consistent_unify(lhs, rhs, SourceLoc{});
    }

    bool bind_var(TypeId var, TypeId val) {
        if (occurs_check(var, val)) return false;
        subst[var.index()] = val;
        return true;
    }

    bool occurs_check(TypeId var, TypeId ty) {
        // 遍历 ty 检查 var 是否在其中
        // 避免无限递归：a == (-> a a)
        if (is_var(ty)) return subst[ty.index()] == var;
        if (is_func(ty)) {
            for (auto arg : reg.func_of(ty).args)
                if (occurs_check(var, arg)) return true;
            return occurs_check(var, reg.func_of(ty).ret);
        }
        return false;
    }

    TypeId normalize(TypeId id) {
        while (is_var(id)) {
            auto next = subst[id.index()];
            if (next == id) return id;
            id = next;
        }
        return id;
    }

    struct SolveResult {
        bool success;
        TypeId result_type;
        std::vector<Diagnostic> errors;  // 含 blame 信息
    };

    SolveResult solve(std::vector<Constraint>& constraints) {
        for (auto& c : constraints) {
            bool ok = false;
            switch (c.kind) {
                case EQUAL: ok = unify(c.lhs, c.rhs); break;
                case SUBTYPE: ok = check_subtype(c.lhs, c.rhs); break;
                case INSTANTIATE: ok = instantiate(c.lhs, c.rhs); break;
            }
            if (!ok) {
                return {false, TypeId::invalid(), {
                    Diagnostic{
                        .severity = Severity::Error,
                        .code = "type-mismatch",
                        .message = format_type_error(c.lhs, c.rhs),
                        .loc = c.loc,
                    }
                }};
            }
        }
        return {true, apply_subst(type_var), {}};
    }
};
```

### 14.3 Coercion 插入

```cpp
// CoercionInsertionPass
// 遍历 TypedPhase AST，在类型边界插入 CoercionNode

enum class CoercionKind {
    ID,             // 恒等（同类型）
    ANY_TO_INT,     // 从 Any 检查 → Int
    INT_TO_ANY,     // Int → Any（擦除）
    ANY_TO_STR,     // Any → String
    STR_TO_ANY,     // String → Any
    FUNC_WRAP,      // 函数边界包装
};

struct CoercionInfo {
    CoercionKind kind;
    TypeId from;
    TypeId to;
    bool runtime_check;  // true → 运行时检查
};

class CoercionPass {
    Expr* insert_coercion(Expr* e, TypeId from, TypeId to, SourceLoc loc) {
        if (from == to) return e;  // ID: 无操作
        if (to == TypeId::Any() && from.is_concrete())
            return e;  // 擦除：静态→动态
        
        if (from == TypeId::Any()) {
            // 动态→静态：运行时检查
            return arena->create<CoercionNode>(
                e, ANY_CHECK(to), loc
            );
        }
        
        // 函数边界包装
        if (is_func_type(from) && is_func_type(to)) {
            return insert_func_coercion(e, from, to, loc);
        }

        return e;  // 其他：暂不处理
    }

    Expr* insert_func_coercion(Expr* f, TypeId from, TypeId to, SourceLoc loc) {
        // 生成：lambda (x) (wrap (f (unwrap x)))
        // unwrap: 参数类型 Any→T
        // wrap: 返回类型 T→Any
        auto& f_from = reg.func_of(from);
        auto& f_to = reg.func_of(to);
        
        std::vector<Expr*> params;
        for (size_t i = 0; i < f_to.args.size(); i++) {
            auto param = arena->create<VarNode>("__coerce_" + i);
            auto unwrapped = insert_coercion(param, f_to.args[i], f_from.args[i], loc);
            params.push_back(unwrapped);
        }
        auto call = arena->create<CallNode>(f, params);
        auto result = insert_coercion(call, f_from.ret, f_to.ret, loc);
        return arena->create<LambdaNode>(params, result);
    }
};
```

---

## 15. 测试计划

### 15.1 基础类型测试（L6.1）

```scheme
;; valid — 编译期通过
(: x Int)
(let ([x : Int 42]) x)          → 42

;; valid — 类型推断
(+ 1 2)                         → 3

;; invalid — 类型错误
(let ([x : Int "hello"]) x)     → TypeError: expected Int, got String
(: (lambda (x) (+ x 1))) Int)   → (-> Int Int) ✓
((: f (-> Int Int) (lambda (x) (+ x 1))) 42)  → 43
```

### 15.2 渐进测试（L6.3）

```scheme
;; 混合代码
(let ([x : Any 42]) x)          → 42
((: f (-> Int Int) (lambda (x) x)) : Any 42)  → 42 (coercion 边界)
((: f (-> Int Int) (lambda (x) x)) : Any "hi") → TypeError (运行时)

;; cast
(cast 42 : Int)                 → 42 (编译期 no-op)
(cast "hello" : Int)            → TypeError (运行时)
```

### 15.3 Occurrence Typing 测试（L6.4）

```scheme
;; occurrence typing
(let ([x : Any "hello"])
  (if (string? x)
      (string-append x "!")     ;; x : String → 编译通过
      x))                        ;; x : not(String) → 编译通过

;; 谓词约束链
(let ([x : Any 42])
  (cond [(number? x) (+ x 1)]   ;; x : Int → 通过
        [(string? x) x]         ;; x : String → 通过（不可达，但类型 ok）
        [else #f]))
```

### 15.4 查询集成测试（L6.5）

```scheme
;; 类型 clause 查询
(query (node-type Call) (return-type Int))

;; 无标注查询
(query (node-type Variable) (no-annotation? #t))

;; 自动修复
(query-and-fix
  (match (Call (op +) (arg (type String) . a) (arg (type Int) . b)))
  (suggest (string-append a (number->string b))))
```

### 15.5 AI 场景测试

```scheme
;; AI 生成代码（无类型）
(lambda (x y) (+ x y))          → 推断为 (-> Int Int Int)

;; AI 生成 + 用户标注
(: add (-> Int Int Int))
(lambda (x y) (+ x y))          → 通过

;; AI 错误修复
;; 原: (string-append "hello" 42)
;; → 类型错误: expected String, got Int
;; → AI query-and-fix: (string-append "hello" (number->string 42))
```

### 15.6 性能基准

```
Benchmark: 混合代码调用
  100% typed:      ~1.0x (基线, 零开销)
  100% dynamic:     ~1.0x (基线, 无类型检查)
  50/50 mixed:     <1.1x (目标: <10% 边界开销)
  Worst-case:      <10x (Muehlboeck 论文保证 <10%)
```

---

## 16. 里程碑展开

### L6 逐日 Sprint 计划

```
Day  Dev      内容                       交付物
───  ───────  ──────────────────────     ───────────────────────
 1   TypeId   TypeId/TypeRegistry 骨架    src/core/type.ixx
              TypeInfo struct             src/core/type_info.ixx
              P2996 layout validation     reflect/type_validate.hh

 2   Parser   Racket :Type 标注解析       lang/private/types.rkt
              S-表达式类型计算器           lang/private/type-check.rkt
              ABF TAG-TYPE (0x0F)       ABF 节点输出

 3   Checker  TypeChecker Pass 骨架      src/compiler/type_checker.ixx
              基础类型检查 (Int/Bool)     TypeChecker::synthesize
              (+ "a" 1) → TypeError

 4   Inference Bi-dir 推断 + 约束收集     ConstraintCollector
              Unification 求解           Unifier
              let-polymorphism 基础       (let [(x Int) ...] ...)

 5   Query    Query 引擎扩展 clause       query: return-type / has-type?
              type-resolution pass
              (query (return-type Int))

 6   Gradual  CoercionNode + CastOp IR   CoercionInsertionPass
              Any 边界检查                (cast x : Int) 运行时检查
              (cast "hello" : Int) →  Error

 7   Polish   Occurrence typing          if 分支类型细化
              Blame 位置                  type errors with Loc
              Benchmark                  性能基线数据
```

---

## 17. 当前状态跟踪

```
里程碑             预计日期    实际日期    红线数量    状态
──────────────────  ────────  ────────  ────────  ────────
v0.1 设计草案       05-12     05-12      —         ✅
v0.2 形式规则补充     05-12     ←NOW      12        🔨
L6.1 TypeId 骨架     05-13     —          3        ⬜
L6.2 解析器扩展      05-14     —          5        ⬜
L6.3 TypeChecker    05-15     —          8        ⬜
L6.4 推断           05-16     —          6        ⬜
L6.5 Query 扩展     05-17     —          4        ⬜
L6.6 渐进机制        05-19     —          6        ⬜
L6.7 Occurrence     05-20     —          4        ⬜
M2.7 集成测试        05-21     —          10       ⬜
```

### 红线清单（按优先级）

```
P0 — 必须通过才能合并 L6：
[ ] (: x Int) (let ([x : Int 42]) x) → 编译通过
[ ] (+ 1 2) → 推断为 Int
[ ] (+ "hello" 1) → 运行时 TypeError

P1 — L6.3 止：
[ ] (cast 42 : Int) → 编译期 no-op
[ ] (cast "hello" : Int) → 运行时 TypeError
[ ] type? / type-of 基元

P2 — L6.5 止：
[ ] (query (node-type Call) (return-type Int)) → 过滤
[ ] query-and-fix: 类型错误 → 自动修复
```

---

## 18. 风险矩阵（扩展）

| 风险 | 概率 | 影响 | 等级 | 缓解 |
|------|------|------|------|------|
| Unification 在模块环境中遇到 P2996 冲突 | 低 | 高 | 中 | reflect/ 隔离 |
| Occurrence typing 在 letrec 循环中不可判定 | 中 | 中 | 中 | 深度限制 |
| Coercion 插入导致 IR 膨胀 >2x | 低 | 中 | 低 | Dead cast elimination pass |
| AI 生成代码类型标注过于稀疏 | 高 | 中 | 中 | inference 回退 |
| C++26 module + gradual RTTI 编译时间 | 中 | 低 | 低 | 增量编译 + cache |
| 与 M2 QueryEngine 接口冲突 | 低 | 高 | 高 | L6.2 Day 1 与 M2 对齐接口契约（type clause 签名、type-resolution pass 签名）。Day 2 写联合测试验证 Query 和 TypeChecker 互操作。|

---

## 20. 线性所有权模型设计（M4 规划）

**版本**：v0.1（草案）
**状态**：设计阶段
**目标阶段**：M4

### 20.1 设计目标与原则

未来 Agent AI OS 对资源安全的要求远高于传统 OS。线性所有权模型的目标是：

- 在类型层面防止 double-free、use-after-free、数据竞争
- 支持 AI Agent 安全地创建、传递、释放系统资源
- 与 Sound Gradual Typing 良好兼容
- 保持 Aura “最小核心 + 宏生长” 的哲学

**关键决策**：
- **优先实现 Affine Ownership**（最多使用一次），而非严格 Linear（必须使用一次）
- 采用 Move 语义 为主，显式借用为辅
- 利用 C++26 std::move + RAII + P2996 实现零成本抽象

### 20.2 核心概念

| 概念 | 含义 | Aura 表示 | 对应 C++ 概念 |
|---------------|-----------------------------------|---------------|-------------------|
| Owned | 拥有唯一所有权 | (Linear T) | Linear<T> |
| Moved | 所有权已转移 | — | moved = true |
| Borrow | 临时共享引用（不可变） | (& T) | Borrow<T> |
| Mut Borrow| 临时独占可变引用 | (&mut T) | MutBorrow<T> |
| Drop | 离开作用域自动释放资源 | defdrop | 析构函数 |

### 20.3 语法设计

```scheme
;; 创建线性资源
(define (open-file path)
  (Linear (FileHandle path)))

;; 移动所有权
(let ([f (open-file "log.txt")])
  (write-line f "Hello Aura")
  ; (write-line f "again") ; 编译错误：f 已被移动
)

;; 不可变借用
(let ([f (open-file "config.txt")])
  (let ([view (& f)])
    (read-config view))
  (write-line f "updated")) ; 仍可使用

;; 可变借用
(let ([f (open-file "data.txt")])
  (let ([mut-view (&mut f)])
    (append-line mut-view "new line"))
  (close f))
```

辅助宏：

```scheme
(move expr)       ; 显式移动
(borrow expr)     ; 不可变借用
(mut-borrow expr) ; 可变借用
```

### 20.4 类型规则（核心）

在 TypeChecker 中新增 **Ownership Pass**，规则如下：

**基本规则**：
- (Linear T) 类型的值默认**移动语义**
- 变量使用后标记为 Moved，再次使用报错
- (& x) 要求 x 当前为 Owned 状态，创建借用计数
- (&mut x) 要求当前无活跃借用，且 x 为 Owned

**约束扩展**：

```cpp
enum class OwnershipKind {
    MustMove,      // 必须移动（消耗所有权）
    Borrowed,      // 已借用
    MutBorrowed,   // 可变借用中
    Owned          // 拥有所有权
};
```

完整形式规则见附录 `aura_typesystem_formal.md` §15（规划）。

### 20.5 与现有类型系统集成

| 现有机制 | 集成方式 |
|----------------------|----------|
| TypedPhase | 新增 OwnershipInfo 扩展字段（state, borrow_count） |
| TypeEnv | 增加 ownership_state 映射 |
| CoercionInsertionPass | Linear<T> ↔ Any 之间插入运行时检查 |
| QueryEngine | 支持 (linear-owned? node)、(has-active-borrow? node) 查询 |
| Hyperstatic Scope | 全局线性资源不可重复绑定 |

### 20.6 C++26 后端实现

```cpp
// aura/linear.ixx
module aura.core.linear;

export template <typename T>
struct Linear {
    T value;
    bool moved = false;

    Linear(T&& v) : value(std::move(v)) {}

    Linear(Linear&& other) noexcept
        : value(std::move(other.value)), moved(other.moved) {
        other.moved = true;
    }

    ~Linear() {
        if (!moved) {
            drop(std::move(value));  // 由 Aura 宏生成
        }
    }
};

export template <typename T>
struct Borrow {
    T* ptr;
    // TODO: 生命周期验证
};
```

**P2996 优化**：编译期验证 moved 状态，自动生成 drop 特化。

### 20.7 渐进类型兼容

```scheme
;; 动态模式（默认）
(let ([f (open-file "a.txt")])  ; 运行时引用计数
  (write f "hello"))

;; 静态线性模式
(let ([f : (Linear File) (open-file "b.txt")])
  (write f "hello")       ; 编译期保证
  ; (write f "world")    ; 错误：f 已被移动
)
```

### 20.8 实现路线图（M4）

| 子阶段 | 内容 | 预计工期 |
|----------|--------------------------------|----------|
| M4.1 | Linear<T> + move 基础支持 | 5 天 |
| M4.2 | 不可变借用 (& T) | 4 天 |
| M4.3 | 可变借用 (&mut T) | 5 天 |
| M4.4 | defdrop 宏 + P2996 验证 | 3 天 |
| M4.5 | QueryEngine 集成 + 测试 | 4 天 |

**红线测试**：
- `(+ (move x) (move x))` → 编译错误
- 借用结束后仍可使用原变量
- Drop 自动调用验证

### 20.9 风险与缓解措施

| 风险 | 影响 | 缓解 |
|------|------|------|
| 所有权语义与宏系统交互复杂 | 高 | Affine 优先，逐步增强 |
| 借用检查性能开销 | 中 | 编译期静态检查，运行时零成本 |
| 与 gradual typing 的 Any 边界 | 中 | Linear<T> ↔ Any 运行时计数降级 |
| C++ RAII 与 Aura GC 并存冲突 | 低 | Linear<T> 明确直接调用析构，不经过 GC |
| 开发者体验陡峭 | 低 | 默认动态 + 可选静态，渐进采用 |

---

## 19. 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v0.1 | 2026-05-12 | 初始草案（设计哲学、类型语言、Phased 实现、AI 集成） |
| v0.2 | 2026-05-12 | 补充形式规则、约束求解算法、测试计划、里程碑展开、风险矩阵 |
| v0.3 | 2026-05-12 | 修复 §13.3 consistent equality (non-transitive)、§13.4 value restriction、§13.5 GG 精确表述、§13.6 occurrence propagation 表、§14.2 consistent_unify。提取 formal 附录。|
| v0.4 | 2026-05-12 | 新增 §20 线性所有权模型设计（M4 规划），Affine + Borrow + Move，C++26 Linear<T> |

