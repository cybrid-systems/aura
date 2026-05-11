# 异常处理机制设计

## 目标

用 `std::expected<T, E>` 替代当前的错误处理模式（`bool success + string error` 结构体、裸指针 null 检查、异常丢弃）。

## 当前错误模式

```cpp
// 1. 结果 + 错误字符串混装
struct EvalResult { bool success; int64_t int_value; std::string error; };

// 2. ParseResult 也是类似
struct ParseResult { bool success; Expr* root; ASTArena* arena; };

// 3. 部分位置直接抛异常
throw std::bad_alloc();
```

## 设计方案

### 错误类型

```cpp
namespace aura::diag {

enum class ErrorKind {
    // Parse
    ParseError,
    UnexpectedToken,
    UnterminatedSExpr,
    // Eval
    UnboundVariable,
    DivisionByZero,
    InvalidClosure,
    ArityMismatch,
    // IR
    IRCorruption,
    IRNoReturn,
    IRTypeError,
    // Internal
    InternalError,
    OutOfMemory,
};

struct SourceLocation {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t file_id = 0;
};

// Structured diagnostic — not just a string
struct Diagnostic {
    ErrorKind kind;
    std::string message;
    SourceLocation location;
    // Support chaining: "while evaluating (call f x): unbound x"
    std::vector<std::string> context_stack;

    Diagnostic& context(std::string_view ctx) {
        context_stack.push_back(std::string(ctx));
        return *this;
    }

    // Human-readable format
    std::string format() const;
};

// Alias for the main result type
template <typename T>
using Result = std::expected<T, Diagnostic>;

} // namespace aura::diag
```

### 迁移路径

#### Phase 1：定义核心类型（当前步骤）

```
src/
└── compiler/
    └── diag.ixx  ← 新增
```

```cpp
export module aura.diag;
import std;

namespace aura::diag {

export enum class ErrorKind { ... };
export struct SourceLocation { ... };
export struct Diagnostic { ... };
export template <typename T> using Result = std::expected<T, Diagnostic>;

} // namespace aura::diag
```

#### Phase 2：替换 Parser

当前：
```cpp
struct ParseResult { bool success; Expr* root; };
```

改为：
```cpp
auto parse(std::string_view input) -> Result<Expr*>;
```

好处：Parse 失败时携带准确的源位置和错误种类。

#### Phase 3：替换 Evaluator（树遍历器）

当前：
```cpp
struct EvalResult { bool success; int64_t value; std::string error; };
EvalResult eval_in(const Expr*, const Env&);
```

改为：
```cpp
auto eval_in(const Expr*, const Env&) -> Result<int64_t>;
```

**关键变更**：
- `EvalResult` 删除，全局替换为 `Result<int64_t>`
- `Evaluator::eval` → `Result<int64_t>`
- 原 `.success` 检查 → `.has_value()`
- 原 `.error` 读取 → `.error().format()`

#### Phase 4：替换 IR 管线

当前：
```cpp
// IRInterpreter 返回 EvalResult
// ArityChecker 用 ArityCheckResult + vector<ArityDiagnostic>
// ComputeKindAnalysis 用 ComputeKindResult + bool valid
```

改为：
- `IRInterpreter::execute()` → `Result<int64_t>`
- `ArityChecker::check()` → `Result<void>`（或直接报错列表）
- `ComputeKindAnalysis::analyze()` → 保持纯分析，不返回 Result（有 valid flag 但本质上不失败）

#### Phase 5：错误语境（context 链）

```cpp
// 错误传播时自动携带调用链
auto callee = eval_in(node.function, env);
if (!callee) return callee;  // 自动向上传播

// 或添加语境:
auto result = eval_in(node.body, ne);
if (!result) return std::unexpected(result.error().context(
    std::format("while binding {}", node.name)));
```

输出效果：
```
error: unbound variable: z
  while binding (lambda (x y) (+ x y z))
  while evaluating (let ((f (lambda (x y) (+ x y z)))) (f 3 4))
```

### 不在范围内的项

| 项目 | 理由 |
|------|------|
| `std::cerr` / `stderr` 输出 | 保持 `std::println(std::cerr, ...)` 用于 CLI 终端输出 |
| 断言式错误 (`assert`) | 仅用于内部不变式违反，非用户可见错误 |
| 内存分配失败 | `std::bad_alloc` 已够用，不需要包装 |

## 设计原则

1. **错误类型化**：每个错误有清晰的 `ErrorKind`，使用者可按 kind 匹配处理
2. **位置精准**：尽量携带源位置信息
3. **语境层级**：错误在传播中追加调用语境，形成清晰的回溯链
4. **零运行时开销**：`std::expected` 在无错误路径上无堆分配
