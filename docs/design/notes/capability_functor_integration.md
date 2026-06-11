# Capability + Functor 集成设计

> 将 Capability Effects（#9）与 Functor 泛型模块结合，
> 实现带安全边界的可复用模块系统。

---

## 动机

当前两个独立的能力：

```
Functor:    (define-module (Stack T) ...)    → (Stack Int)
Capability: !IO, !Mutation, !FileRead, !FileWrite, !Network, !AgentMsg
```

但无法表达「这个模块需要什么能力才能实例化」。
例如，一个数据库模块需要 `!FileRead + !FileWrite` 才能工作，
一个网络客户端需要 `!Network`。

**目标：** 让 Functor 可以声明所需的 Capability，实例化时检查
调用方是否持有这些 Capability。

---

## 语法设计

### 1. 带 Capability 的 Functor 定义

```scheme
(define-module (PersistentDB path (cap : Capability))
  (:require-file FileRead FileWrite)
  (export query save)
  (define (query sql)    ...)  ;; 需要 FileRead
  (define (save data)    ...)  ;; 需要 FileWrite
  (define (open)         ...)  ;; IO
```

### 2. Capability 作为类型参数

```scheme
;; 泛型 + Capability 约束
(define-module (SecureService T (cap : Capability))
  (:require Network)
  (export call)
  (define (call x)     (send-request x))
  (define (process x)  (+ x 1)))
```

### 3. 实例化

```scheme
(define db (PersistentDB "/data/db" FileReadWrite))
;; 编译期检查: 调用方持有 FileReadWrite → 允许

(define svc (SecureService Int Network))
;; 编译期检查: 调用方持有 Network → 允许
```

---

## 类型系统设计

### 1. Effect 类型构造器

```
Effect ::= !Name                ;; 如 !IO
         | !Name[Type]          ;; 如 !Mutation[NodeId]
         | Capability           ;; effect 集合
```

### 2. Functor 签名中的 Effect

```scheme
;; 当前
PersistentDB: ∀. (String) -> Module{...}

;; 未来（带 Capability）
PersistentDB: ∀cap. (String, cap) -> Module{...}
              where cap ⊇ {FileRead, FileWrite}
```

### 3. TypeTag 扩展

当前已有 `TypeTag::EFFECT`，需要新增 `TypeTag::CAPABILITY`：

```cpp
export enum class TypeTag : uint8_t {
    // ... existing ...
    EFFECT,      // !IO, !FileRead
    CAPABILITY,  // 能力集合 {FileRead, FileWrite}
};
```

### 4. CapabilityType 结构体

```cpp
export struct CapabilityType {
    std::vector<std::string> effects;  // 包含的 effect 列表
    bool is_unrestricted = false;      // true = 允许所有 effect
};
```

---

## 类型检查规则

### 规则 1: Capability 包含检查

```
ctx ⊢ expr : A ! eff
ctx ⊢ eff ⊆ allowed_effects
──────────────────────────────────
ctx ⊢ expr : A
```

如果表达式的 effect `eff` 不在当前上下文允许的 `allowed_effects` 中，
报类型错误。

### 规则 2: Functor 实例化 Capability 检查

```
(define-module (M T (cap : Capability)) ...)
  cap 的类型是 Capability{FileRead, FileWrite}

(M Int FileReadWrite)  ← 检查 FileReadWrite ⊇ {FileRead, FileWrite}
  → 允许

(M Int Network)  ← 检查 Network ⊇ {FileRead, FileWrite}
  → 拒绝: "missing capabilities: FileRead, FileWrite"
```

### 规则 3: 效果传播

```scheme
(define (unsafe-fn x)
  (mutate:rebind "f" "(lambda (y) y)"))  ;; !Mutation

(define (safe-fn x)
  (+ x 1))                                ;; 无效果
```

`safe-fn` 的类型: `Int -> Int`
`unsafe-fn` 的类型: `Int -> Int ! Mutation`

---

## Evaluator 变化

### 1. with-capability 原语

```scheme
(with-capability FileRead
  (read-file "/data/file"))
```

在指定的 capability 上下文中执行代码：

```cpp
primitives_.add("with-capability", [this](const auto& a) -> EvalValue {
    // a[0] = capability name (string)
    // a[1..n] = body expressions
    // TODO: push capability context, eval body, pop
});
```

### 2. Capability 上下文栈

```cpp
std::vector<std::vector<std::string>> capability_stack_;
// 每层包含当前作用域允许的 effect 名称
```

---

## .aura-type 签名

### 带 Capability 的模块签名

```
; mylib.aura-type
PersistentDB: (String, Capability) -> Module{
    query: String -> String ! FileRead,
    save: String -> Void ! FileWrite,
    open: Void -> Void ! IO
}
  require: {FileRead, FileWrite}
```

### 实例化后的签名

```scheme
(define db (PersistentDB "/data/db" FileReadWrite))
;; → db.aura-type
;;   query: String -> String ! FileRead
;;   save: String -> Void ! FileWrite
;;   open: Void -> Void ! IO
```

---

## 实现阶段

| 阶段 | 内容 | 涉及文件 | 预计时间 |
|------|------|----------|----------|
| P0 | CapabilityType + TypeTag::CAPABILITY | `type.ixx`, `type_impl.cpp` | 1h |
| P1 | `with-capability` 原语 + 上下文栈 | `evaluator_impl.cpp` | 2h |
| P2 | Functor 参数类型扩展支持 Capability | `parser_impl.cpp`, `evaluator_impl.cpp` | 3h |
| P3 | 编译期 Capability 包含检查 | `type_checker_impl.cpp` | 3h |
| P4 | .aura-type 签名 + 测试 | `tests/` | 2h |

---

## 测试用例

```scheme
;; 1. 基本 capability 检查
(with-capability FileRead
  (read-file "/data/file"))  ;; → ok

(with-capability FileWrite
  (read-file "/data/file"))  ;; → type error: missing FileRead

;; 2. 带 capability 的 functor
(define-module (DB path (cap : Capability))
  (:require FileRead FileWrite)
  (define (query sql) (read-file path))
  (define (save d)    (write-file path d)))

(define mydb (DB "/db" FileReadWrite))  ;; → ok
(define mydb2 (DB "/db" Network))       ;; → type error: missing FileRead, FileWrite

;; 3. 多层 capability
(with-capability (FileRead FileWrite)
  (define-module (SecureStore T (cap : Capability))
    (:require FileWrite)
    (define (save x) (write-file "/store" x)))
  (define store (SecureStore Int FileWrite)))  ;; → ok
```
