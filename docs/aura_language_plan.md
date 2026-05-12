# Ghuloum Steps 16-35: 语言内核增量完善计划

**版本**：v1.0
**日期**：2026-05-12
**方法**：《An Incremental Approach to Compiler Construction》— 每步增加一个最小功能，系统始终可运行可测试。

---

## 现状：Step 1-15 已就绪 + Pre-Step 0 ✅

```
Step  C++  功能
────  ───  ──────────────────────────────
1-8   ✅   整数/变量/lambda/if/let/letrec/算术/比较
9-14  ✅   布尔/序对/begin/set!/quote/cond → 语言核心语法
15    ✅   defmacro 模板替换
Pre0  ✅   () 空列表 → 0 sentinel， (cons 1 (cons 2 ())) 不再 crash
```

**可用的值类型**：`int64_t`（布尔用 0/1，序对用 sentinel，空列表用 0）
**可用的过程**：`+ - * / = < > not and or eq? cons car cdr pair? null?`

---

## 按 Review 反馈调整的计划变更

| 反馈 | 调整 |
|------|------|
| Pre-Step 0: 先修空列表 bug | ✅ 已修复，`()` → 0 |
| list 做成 primitive 而非宏 | 采纳，L2 Step 19 改为 primitive |
| equal? 必须在 L2 完成 | 采纳 |
| 增加 list? 谓词 | 采纳 |
| L3 I/O 拆分 | 采纳：display/newline 先做，read/error 后面 |
| AI 友好特性（AST 查询暴露） | 增加 Step 22.5 |
| 字符串放 arena 而非独立 heap | 采纳 |

```
Step  C++  功能
────  ───  ──────────────────────────────
1-8   ✅   整数/变量/lambda/if/let/letrec/算术/比较
9-14  ✅   布尔/序对/begin/set!/quote/cond → 语言核心语法
15    ✅   defmacro 模板替换
```

**可用的值类型**：`int64_t`（布尔用 0/1，序对用 sentinel）
**可用的过程**：`+ - * / = < > not and or eq? cons car cdr pair? null?`

---

## 总览

```
Pre-Step 0: 空列表修复     (0.5天) ✅ 完成
Phase L1: 字符串          (3天)  Step 16-18
Phase L2: 列表库          (3天)  Step 19-22
Phase L3a: 输出基元       (1天)  Step 23-24
Phase L3b: 输入+错误      (2天)  Step 25-26
Phase L4: 数值扩展        (2天)  Step 26-27
Phase L5: 向量与复合数据  (3天)  Step 28-30
Phase L6: 类型系统启蒙    (5天)  Step 31-35

总计: 19 步, 约 19 天
```

---

## Phase L1: 字符串 (Step 16-18, 3 天)

### Step 16: 字符串字面量 — 1 天

**变更**：
- 解析器：`"hello"` → 读入双引号字符串 → 返回 `LiteralIntNode`？不对，需要字符串节点

**问题**：当前整数值用 `int64_t` 存，字符串需要新的值类型。

**设计方案**：引入 `EvalValue` variant：

```cpp
using EvalValue = std::variant<std::int64_t, std::string>;
```

但这会影响整个 tree-walker 和 IR 解释器。更实际的方案：**值继续用 `int64_t`，字符串用类似序对的 sentinel 机制**，在 `Evaluator` 里放一个 `StringPool`。

或者更简单的：直接用 **Racket 风格的 tagged pointer** —— 字符串作为全局 string pool 的索引。

```
(方案 A: EvalValue variant) — 正确但改动大
(方案 B: StringPool + sentinel) — 实用，和序对模式一致
```

**推荐方案 B**：

```cpp
constexpr std::int64_t STRING_SENTINEL = 0x8000000;

// Evaluator 新增:
std::vector<std::string> string_heap_;
// cons 模式复用了：发送值 = SENTINEL + index
```

**红线**：
```scheme
"hello"              → 指向 StringPool 的 sentinel 值
(string? "hello")    → #t
```

### Step 17: 字符串基元 — 1 天

```scheme
(string-append "a" "b")    → "ab"
(string-length "hello")    → 5
(string-ref "hello" 0)     → 104  (ASCII 'h')
(substring "hello" 1 3)    → "el"
```

**实现**：全部作为 `Evaluator` 上的 primitives，操作 `string_heap_`。

### Step 18: 字符与字符串转换 — 1 天

```scheme
(string->number "42")      → 42
(number->string 42)        → "42" 的 sentinel
(symbol->string 'x)        → "x" 的 sentinel
(string->symbol "x")       → VariableNode("x") 求值
(string=? "a" "b")         → #f
(string<? "a" "b")         → #t
```

---

## Phase L2: 列表库 (Step 19-22, 3 天)

### Step 19: 列表构造 — 1 天

**当前**：`(cons 1 (cons 2 ()))` 会 crash（`()` 作为空列表处理有 bug）。

**修复**：`()` 解析为 0 (null sentinel)，`null?` 检查 `== 0`。但是 `()` 的解析器当前可能不工作。

**新增**：
```scheme
(list 1 2 3)               → (cons 1 (cons 2 (cons 3 ())))  ← 宏
(null? ())                 → #t  
(cons 1 (cons 2 ()))       → 不 crash
```

`list` 可以用 defmacro 实现：`(defmacro list xs xs)` — 不对，`(list 1 2 3)` 要展开为 `(cons 1 (cons 2 (cons 3 ())))`。这需要递归宏。

实际上 `list` 更适合作为 primitive（直接操作变参）。

### Step 20: 列表查询 — 1 天

```scheme
(length (list 1 2 3))      → 3
(list-ref (list 10 20 30) 1) → 20
(member 2 (list 1 2 3))    → (2 3)  或 #f
(assoc 'x (list (cons 'x 1) (cons 'y 2))) → (x . 1)
```

### Step 21: 列表变换 — 1 天

```scheme
(append (list 1 2) (list 3 4))  → (1 2 3 4)
(reverse (list 1 2 3))          → (3 2 1)
(map (lambda (x) (* x 2)) (list 1 2 3)) → (2 4 6)
(filter (lambda (x) (> x 1)) (list 1 2 3)) → (2 3)
```

### Step 22.5: AST 反射（AI 友好）— 1 天

```scheme
(ast-node? x)              → 判断 x 是否为持久 AST 节点
(ast-children x)           → 获取节点的子节点列表
(ast-type x)               → 返回节点类型标识符 (如 'Call)
(query-ast pattern)        → 直接调用 QueryEngine (已有的 TagIndex/SymRefIndex)
```

**实现**：调用现有的 `FlatAST` 和 `QueryEngine`，通过 `--serve` IPC 暴露。
这是 Aura 作为 "AI-native language" 的差异化能力。

### Step 26: 相等比较 — 1 天

```scheme
(equal? (list 1 2) (list 1 2))  → #t
(equal? "hello" "hello")        → #t
```

`equal?` 递归比较序对和字符串的结构相等性。

---

## Phase L3: I/O 与控制 (Step 23-25, 3 天)

### Step 23: 输出 — 1 天

```scheme
(display "hello")          → 打印到 stdout，返回 #t
(display 42)               → 打印 "42"
(newline)                  → 打印换行
(write (list 1 2 3))       → 打印可读形式
```

**实现**：`display` 和 `newline` 作为 primitives，直接调用 `std::print` / `std::println`。

### Step 26: 输入 — 1 天

```scheme
(read)                     → 从 stdin 读一个 S-表达式
(read-line)                → 从 stdin 读一行字符串
(eof-object? (read))       → 判断是否读到 EOF
```

**实现**：`read` 调用当前的解析器，`read-line` 读取一行作为字符串。

### Step 26: 错误处理 — 1 天

```scheme
(error "msg")              → 抛出 Diagnostic 错误
(error 'my-error "msg")    → 带类型的错误
(assert (> x 0))           → 条件不满足时 error
```

**实现**：`error` 直接返回 `std::unexpected(Diagnostic{...})`。

---

## Phase L4: 数值扩展 (Step 26-27, 2 天)

### Step 26: 数值运算 — 1 天

```scheme
(+ 1 2 3)                  → 6  (变参)
(* 1 2 3)                  → 6
(- 10 3 2)                 → 5
(/ 10 2 5)                 → 1
(modulo 10 3)              → 1
(quotient 10 3)            → 3
(remainder 10 3)           → 1
(gcd 12 8)                 → 4
(lcm 4 6)                  → 12
(abs -5)                   → 5
(max 3 7 2)                → 7
(min 3 7 2)                → 2
```

**当前**：`+ - * /` 只接受 2 个参数。需要改为变参。

### Step 27: 浮点数 — 1 天

```scheme
3.14                       → 浮点字面量
(+ 1.5 2.5)                → 4.0
(= 3.0 3)                  → #t
```

**挑战**：当前值系统是 `int64_t`。浮点数需要一个新值类型：
- `EvalValue` variant 扩展 `double`
- 或者 tagged pointer
- 或者把 1.5 表示为 scaled integer（不推荐）

**推荐**：引入 `EvalValue` variant = `int64_t | double | string_ref | pair_ref`。

**这是一个重大重构**，但迟早要做。浮点数是正确的催化剂。

---

## Phase L5: 向量与复合数据 (Step 28-30, 3 天)

### Step 28: 向量基元 — 1 天

```scheme
(make-vector 3)            → #(0 0 0)
(make-vector 3 'x)         → #(x x x)
(vector-ref #(1 2 3) 1)    → 2
(vector-set! v 1 99)       → 修改
(vector-length #(1 2 3))   → 3
(vector->list #(1 2 3))    → (1 2 3)
(list->vector '(1 2 3))    → #(1 2 3)
```

### Step 29: 记录/结构体 (通过宏) — 1 天

基于 defmacro + 向量实现 `define-struct`：

```scheme
(defmacro define-struct (name . fields)
  `(begin
     (defmacro (,(symbol-append 'make- name) . args)
       `(vector ',(unquote 'name) ,@args))
     ...))

(define-struct point (x y))
(make-point 10 20)         → #(point 10 20)
(point-x #(point 10 20))   → 10
```

### Step 30: 类型谓词系统 — 1 天

```scheme
(integer? 42)              → #t
(float? 3.14)              → #t
(string? "hello")          → #t
(pair? (cons 1 2))          → #t
(null? ())                 → #t
(vector? #(1 2 3))         → #t
(boolean? #t)              → #t
(symbol? 'x)               → #t
(number? 42)               → #t
(number? 3.14)             → #t
```

---

## Phase L6: 类型系统启蒙 (Step 31-35, 5 天)

### Step 31: EvalValue variant — 1 天

当前：`int64_t` 通用值
目标：`std::variant<std::int64_t, double, std::string*, Pair*, Vector*, bool>`

**影响面**：
- `frontend_impl.cpp` — `eval_in` 返回 `EvalResult` (= `expected<EvalValue, Diagnostic>`)
- `Primitives` — `PrimFn` 从 `int64_t(vector<int64_t>)` 改为 `EvalValue(vector<EvalValue>)`  
- `ir_interpreter_impl.cpp` — IR 解释器的 `int64_t` 局部变量
- `CompilerService` — 返回值类型
- `main.cpp` — 输出格式

**这是一个大重构，建议在 Phase L1-L5 验证所有功能后再做。**

### Step 32-35: 进一步类型 (4 天)

根据 Step 31 完成后评估。

---

## 路线图

```
Phase     Steps   内容             红线                     时间
──────    ─────   ────────────     ──────────────────       ────
L1        16-18   字符串            (string-append "a" "b")  3 天
L2        19-22   列表库            (map f (list 1 2 3))     3 天
L3        23-25   I/O + 错误       (display "hello")         3 天
L4        26-27   数值扩展          (+ 1 2 3) → 6             2 天
L5        28-30   向量 + struct    (make-vector 3)            3 天
L6        31-35   类型系统启蒙      EvalValue variant         5 天
───────────────────────────────────────────────────────────
总计      20 步                   语言内核可用               19 天
```

**优先级建议**：

```
立即做:
  L1 Step 16: 字符串字面量 (1 天)
  → 最小可行增量，解决 "hello" 都解析不了的问题

同时做:
  Step 19 的一部分: 修复 (cons 1 (cons 2 ())) 的 crash
  → 当前序对实现有 bug，影响所有列表操作

等 L1 验证后再推:
  L2-L6 → 按顺序
```

---

## 设计约束

1. **每步可编译，每步可测试** — 严格执行 Ghuloum 原则
2. **已有基础设施不改** — ABF、query engine、reflection 保持不变
3. **字符串用 sentinel + string_heap** — 和序对模式一致，不需要改值系统
4. **变参基元逐步改** — `+` 从 2 参数改为变参，不破坏已有测试
5. **EvalValue 推迟到 L6** — 最大重构放最后，先用 sentinel 模式支撑 L1-L5

---

> 语言当前的状态像一栋毛坯房：框架（IR 管线）和智能家居（查询引擎/反射）都装好了，
> 但水管（字符串）和电路（列表库）还没铺。Phase L1-L5 就是铺水电，L6 才是整体装修。
