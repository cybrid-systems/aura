# Functor 泛型模块设计

> 基于 #8 模块类型签名之上的泛型模块系统。

---

## 动机

当前 Aura 的模块系统支持：
- 带前缀/不带前缀的 `import` 和 `require`
- `.aura-type` 签名声明 + 自动加载
- `generate-type-sigs` 自动生成签名
- `check-module-signature` 签名校验
- ABF 缓存嵌入签名

但缺少**模块级泛型**——无法编写一个「带类型参数」的模块，然后为不同
类型实例化。典型场景：

```scheme
;; 每次都要手写
(define (push-int s x) (cons x s))
(define (pop-int s) (cdr s))
(define (push-str s x) (cons x s))
(define (pop-str s) (cdr s))

;; 理想
(define int-stack (Stack Int))
(define str-stack (Stack String))
```

---

## 设计

### 1. 语法

```scheme
(define-module (Stack T)
  (export push pop top empty)
  (define (push s x) (cons x s))
  (define (pop s) (cdr s))
  (define (top s) (car s))
  (define empty ()))

;; 实例化：函数调用风格
(define int-stack (Stack Int))
(define str-stack (Stack String))

;; 使用实例化后的模块
(define s (int-stack:empty))
(define s (int-stack:push s 42))
(int-stack:top s)  ; → 42
```

### 2. 多参数 + 约束

```scheme
(define-module (Map K V)
  (export empty get set)
  (define empty (hash))
  (define (get m k) (hash-ref m k))
  (define (set m k v) (hash-set! m k v)))

(define str-int-map (Map String Int))
```

### 3. 语义

`define-module` 定义的是一个**模块构造函数**：
- 类型参数（`:T`, `:K` 等）在实例化时被具体类型替换
- 模块体在实例化时重新 eval，参数绑定到具体类型
- 每个实例化结果是一个独立模块（独立 Env）
- 实例化结果缓存：相同类型参数返回缓存模块

### 4. 编译模型

```
Phase 1: 解析
  (define-module (Stack T) body...)
  → 存储为 ModuleTemplate{param, body_ast}

Phase 2: 实例化 (Stack Int)
  1. 查找 module_templates_["Stack"]
  2. 创建新 Env，绑定 :T = Int
  3. 在 Env 中 eval body
  4. 应用 export 过滤
  5. 缓存实例化结果（key = ("Stack", Int)）
  6. 返回模块对象

Phase 3: 类型签名
  实例化时生成 .aura-type 签名：
  push: (List Int) Int → List Int
  pop: (List Int) → List Int
```

---

## 实现方案

### 方案 A：Parser 扩展（推荐）

在 `src/parser/parser_impl.cpp` 中新增 `define-module` 特殊形式：

```cpp
if (kw == "define-module")
    return parse_define_module();
```

`parse_define_module` 将 `(define-module (Name :T) body...)` 解析为
新的 AST 节点类型 `NodeTag::DefineModule`。

### 方案 B：Macro 展开（快速原型）

在宏展开或 evaluator 中将 `define-module` 展开为 `define` + 模板
数据结构的组合。不需要修改 parser。

### 选择：方案 A

方案 A 更干净，为未来扩展（约束、kind system）留足空间。

---

## AST 节点

新增 `NodeTag::DefineModule`：

```cpp
DefineModule = 0x16,  // (define-module (Name :T) body...)
  sym_id:    Name
  children:  body 表达式
  params:    类型参数列表（如 [:T, :K, :V]）
```

---

## Evaluator 变化

### 1. 存储模板

```cpp
struct ModuleTemplate {
    std::vector<aura::ast::SymId> type_params; // [:T, :K, ...]
    aura::ast::NodeId body;                     // body AST
};
std::unordered_map<std::string, ModuleTemplate> module_templates_;
```

### 2. 实例化

当 tree walker 遇到 `(Stack Int)` → `Stack` 的值是
`ModuleTemplate` → 实例化路径：

```cpp
// 在 Call 处理器中
auto val = eval_flat(callee_id);
if (is_module_template(val)) {
    auto& tmpl = module_templates_[tmpl_name];
    // 创建新 Env
    Env inst_env(&top_env);
    inst_env.bind(":T", type_arg);  // 绑定类型参数
    // eval body in inst_env
    auto result = eval_flat(body_ast, inst_env);
    // export 过滤
    // 缓存
    return make_module(inst_env);
}
```

### 3. 参数绑定

`:T` 作为一个标识符（Symbol/Variable），在实例化时被 `define` 值
替换。`T` 本身在模块体内的所有引用都被按需替换。

更精确的做法：在 eval body 前，先在环境中绑定 `:T` → `Int`。
当 body 中引用 `:T` 时，它被解析为 `Int`。

---

## Type Checker 变化

### 1. 模块构造函数类型

`Stack` 在类型环境中的类型：
```
Stack: ∀T. Module{push: (List T) T → List T, pop: (List T) → List T, ...}
```

### 2. 实例化类型

`(Stack Int)` 的推断类型：
```
Module{push: (List Int) Int → List Int, pop: (List Int) → List Int, ...}
```

### 3. Module 类型表示

新增 `TypeTag::MODULE`：

```cpp
case TypeTag::MODULE: {
    // (Module sym1:type1 sym2:type2 ...)
    auto* mod_type = module_of(tid);
    // 类型替换 T → Int 并返回新 Module 类型
    return substitute(mod_type, "T", Int);
}
```

---

## 缓存

实例化结果缓存结构：

```cpp
// key = (template_name, [type_arg1, type_arg2, ...])
// value = 实例化后的模块 env + 签名
std::unordered_map<
    std::pair<std::string, std::vector<TypeId>>,
    Env*
> functor_cache_;
```

缓存失效条件：模块模板定义被 `mutate:rebind` 修改。

---

## 与 .aura-type 集成

实例化后自动生成签名：

```scheme
(define int-stack (Stack Int))
;; → int-stack.aura-type:
;;   push: (List Int) Int → List Int
;;   pop: (List Int) → List Int
```

签名生成方式：对实例化后的模块调用 `generate-type-sigs`。

---

## 阶段计划

| 阶段 | 内容 | 涉及文件 |
|------|------|----------|
| P0 | Parser: `define-module` 关键字 + `NodeTag::DefineModule` | `parser_impl.cpp`, `ast.ixx` |
| P1 | Evaluator: `module_templates_` + 存储模板 | `evaluator.ixx`, `evaluator_impl.cpp` |
| P2 | Evaluator: 实例化路径（Call 处理器中） | `evaluator_impl.cpp` |
| P3 | Type checker: `ModuleSig` 类型 + 类型替换 | `type.ixx`, `type_checker_impl.cpp` |
| P4 | 实例化缓存 | `evaluator_impl.cpp` |
| P5 | .aura-type 签名生成 + ABF 嵌入 | `evaluator_impl.cpp`, `service.ixx` |
| P6 | 测试（单参数/多参数/嵌套/约束） | `tests/` |

---

## 测试用例

```scheme
;; 1. 基本单参数
(define-module (Stack T) (export push pop)
  (define (push s x) (cons x s))
  (define (pop s) (cdr s)))
(define int-stack (Stack Int))
(display (int-stack:push () 42))

;; 2. 多参数
(define-module (Pair A B) (export make fst snd)
  (define (make a b) (cons a b))
  (define (fst p) (car p))
  (define (snd p) (cdr p)))

;; 3. 类型约束（未来）
(define-module (Sortable :T (:< Ord))
  (export sort)
  (define (sort lst) ...))

;; 4. 嵌套泛型
(define int-stack-of-sets (Stack (Set Int)))
```

---

## 未解决的问题

1. **`:T` 语法**：当前 Aura 中 `:` 是类型注解关键字。`:T` 作为
   参数可能和现有语法冲突。替代方案：`(T : Type)` 或 `(of T)`。

2. **模块访问语法**：`int-stack:push` 使用 `:` 作为模块成员访问
   运算符（类似 Rust 的 `::`）。但 `:` 已被类型注解占用。

3. **类型参数 vs 值参数**：`(Stack Int)` 中的 `Int` 是类型还是值？
   在实例化时，它作为符号被绑定。类型检查器需要区分类型和值
   参数。

4. **递归模块**：一个 functor 能否引用自身？`(define-module (Stream :T)
   ...)` 内部能否实例化 `(Stream T)`？
