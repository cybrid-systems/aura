# Aura 类型系统 — 形式规则与约束求解（正式附录）

**关联主文档**：[aura_typesystem.md](./aura_typesystem.md)
**核心原则**：Sound Gradual Typing × Homoiconic × Reflective × Queryable — 为 AI 而生
**版本**：v0.3 (2026-05-12)

---

本附录摘自主文档 §13（形式类型规则）和 §14（约束求解算法），保留完整形式语法。
主文档中保留精简摘要。

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

一致性关系（Consistency — 用于混合代码）：
T ~ T                    (自身一致)
T ~ Any                  (动态与任何类型一致)
Any ~ T                  (同上)

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
Γ ⊢ (x e2) : [a ↦ T2]T1'
```

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

**Gradual Guarantee**（渐进定理，来自 Siek et al.）：
```
If e is well-typed under T,
then replacing some T with Any yields a well-typed e' (类型擦除不破坏良类型性)
```

### 13.6 Occurrence Typing 规则（扩展）

```
Γ ⊢ e : Bool    Γ refine_by (e = #t) ⊢ e1 : T
Γ refine_by (e = #f) ⊢ e2 : T
-------------------------------------- T-If-Occurrence
Γ ⊢ (if e e1 e2) : T

其中 refine_by(pred) 对类型环境进行谓词约束：
  (string? x)  →  x 在 then 分支变为 String
  (number? x)  →  x 在 then 分支变为 Int
  (pair? x)    →  x 在 then 分支变为 (Pair Any Any)
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

    bool unify(TypeId lhs, TypeId rhs) {
        lhs = normalize(lhs);
        rhs = normalize(rhs);
        if (lhs == rhs) return true;
        
        // 类型变量 → β 替换
        if (is_var(lhs)) return bind_var(lhs, rhs);
        if (is_var(rhs)) return bind_var(rhs, lhs);

        // 函数类型：分解
        if (is_func(lhs) && is_func(rhs)) {
            auto& f_lhs = reg.func_of(lhs);
            auto& f_rhs = reg.func_of(rhs);
            if (f_lhs.args.size() != f_rhs.args.size()) return false;
            for (size_t i = 0; i < f_lhs.args.size(); i++)
                if (!unify(f_lhs.args[i], f_rhs.args[i])) return false;
            return unify(f_lhs.ret, f_rhs.ret);
        }

        // 基础类型：相等
        return false;  // Int ≠ Bool etc.
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


### §15 线性所有权模型的形式规则

**版本**：v0.1（草案）
**对应设计**：`aura_typesystem.md` §20
**日期**：2026-05-12

#### 15.1 扩展语法

在原有表达式语法基础上新增：

```
e ::= ...
 | (Linear e)     ; 线性包装
 | (move e)       ; 显式移动
 | (& e)          ; 不可变借用
 | (&mut e)       ; 可变借用
 | (drop e)       ; 显式析构（可选）
```

类型扩展：

```
T ::= ...
 | (Linear T)     ; 线性/仿射类型
 | (& T)          ; 不可变借用类型
 | (&mut T)       ; 可变借用类型
```

#### 15.2 判断形式

引入**所有权环境** Ω 来跟踪变量的所有权状态：

```
Γ ; Ω ⊢ e : T
```

其中：
- `Γ`：类型环境（变量 → 类型）
- `Ω`：所有权环境（变量 → 所有权状态）
- `T`：表达式类型

**所有权状态**（`Ω(v)`）：

| 状态 | 含义 | 可操作性 |
|---------------|--------------------------|-------------------|
| Owned | 拥有唯一所有权 | 可移动、可借用 |
| Moved | 所有权已转移 | 不可再使用 |
| Borrowed(n) | 被不可变借用 n 次 | 不可移动 |
| MutBorrowed | 被可变借用 | 不可移动、不可借用 |

#### 15.3 核心类型规则

```
Γ ; Ω ⊢ e : T
---------------------------- T-Linear
Γ ; Ω ⊢ (Linear e) : (Linear T)

Γ ; Ω ⊢ e : (Linear T)    Ω(x) = Owned
------------------------------------------ T-Move
Γ ; Ω[x ↦ Moved] ⊢ (move e) : (Linear T)

Γ ; Ω ⊢ e : (Linear T)    Ω(x) = Owned ∨ Borrowed(_)
---------------------------------------------------- T-Borrow
Γ ; Ω[x ↦ Borrowed(n+1)] ⊢ (& e) : (& T)

Γ ; Ω ⊢ e : (Linear T)    Ω(x) = Owned
------------------------------------------ T-MutBorrow
Γ ; Ω[x ↦ MutBorrowed] ⊢ (&mut e) : (&mut T)

Γ(x) = T    Ω(x) = Owned
------------------------------ T-Var-Linear
Γ ; Ω[x ↦ Moved] ⊢ x : T

Γ ; Ω ⊢ e₁ : (Linear T)    Γ, x:(Linear T) ; Ω[x ↦ Owned] ⊢ e₂ : U
-------------------------------------------------------------------- T-Let-Linear
Γ ; Ω ⊢ (let ([x e₁]) e₂) : U

Γ ; Ω ⊢ e : (Linear T)    Ω(x) = Owned ∨ Moved
-------------------------------------------------- T-Drop
Γ ; Ω ⊢ (drop e) : Void
```

#### 15.4 与现有规则的交互

```
Γ ; Ω ⊢ f : (-> (Linear T) U)    Γ ; Ω ⊢ e : (Linear T)
-------------------------------------------------------------- T-App-Linear
Γ ; Ω' ⊢ (f e) : U (Ω' 标记参数为 Moved)

两个分支的 if 需合并所有权状态（同 Occurrence Typing）：
Ω_merged = Ω₁ ⊔ Ω₂
```

#### 15.5 与 Gradual Typing 的交互

```
Γ ; Ω ⊢ e : T    T ~ (Linear U)
------------------------------------ Gradual-Linear
Γ ; Ω ⊢ e : (Linear U) (插入运行时所有权检查)
```

当类型为 Any 时，线性检查退化为运行时引用计数（与当前动态语义兼容）。

#### 15.6 Soundness 定理草稿（M4 目标）

**Progress**：
若 Γ ; Ω ⊢ e : T 且 e 不是值，则 ∃ e' 使得 e → e'，所有权状态保持一致。

**Preservation**：
若 Γ ; Ω ⊢ e : T 且 e → e'，则 ∃ Ω' 使得 Γ ; Ω' ⊢ e' : T。

**No Double Use**：
线性变量在被移动后无法再次使用（通过 Moved 状态强制）。

**Gradual Guarantee**：
将部分 (Linear T) 替换为 Any 后，程序行为一致（仅可能增加运行时检查）。
