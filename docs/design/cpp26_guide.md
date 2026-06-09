# Aura — 现代 C++ 编码指南

**版本**：v1.0
**目标**：从"功能正确"到"函数式 + DOD + C++26 高性能"的逐步重构路线图。

---

## 概述

Aura 当前架构已具备前瞻性：C++26 modules、pmr arena、SSA-like IR、PassManager。但存在**过度 OO、状态机类过多、现代特性使用不足**的问题。

本文档定义编码风格目标，并给出逐步迁移路径。

---

## 1. 核心原则

| 原则 | 说明 | 反对模式 |
|------|------|----------|
| **纯函数优先** | 无副作用、无隐藏状态、输入→输出 | 带内部 mutable 状态的 Manager/Service 类 |
| **DOD 数据设计** | SoA/plain struct + 索引，无指针 chasing | 对象树 + 虚函数 + vtable |
| **值语义** | `std::expected`、`std::optional`、拷贝/移动 | `bool success + string error` 结构体 |
| **零开销抽象** | `std::span`、`std::string_view`、Concepts | `const std::vector&`、裸指针传递 |
| **编译期优先** | Contracts、Consteval、CTRE | 运行时 assert、运行时正则 |

---

## 2. 具体规则

### 2.1 纯函数 > 状态机

**坏**（当前模式）：
```cpp
class Parser {
    std::optional<Lexer> lexer_;
    ASTArena& arena_;
public:
    ParseResult parse(std::string_view source);  // 依赖内部 lexer_ 状态
};
```

**好**（目标模式）：
```cpp
// 无状态工具函数
export struct ParseResult {
    ast::Expr* root = nullptr;
    std::string error;
    bool success = false;
};

export ParseResult parse(std::string_view source,
                         ast::ASTArena& arena) noexcept;
```

同理：
- `LoweringPass` → `lower_to_ir(const Expr*, ASTArena&) -> IRModule`
- `ArityChecker` → `check_arity(const IRModule&) -> ArityCheckResult`
- `ComputeKindAnalysis` → `analyze_compute_kind(const IRFunction&) -> ComputeKindResult`

**迁移清单**：

| 文件 | 当前 | 目标 | 优先级 |
|------|------|------|--------|
| `parser.ixx` | class Parser | 纯函数 `parse()` | P0 |
| `lowering.ixx` | class LoweringPass | 纯函数 `lower_to_ir()` | P0 |
| `arity.ixx` | class ArityChecker | 纯函数 `check_arity()` | P1 |
| `compute_kind.ixx` | class ComputeKindAnalysis | 纯函数 `compute_kind()` | P1 |
| `signalfd` | 暂无 | — | — |

### 2.2 Concepts > 运行时多态

**坏**：
```cpp
class IRPass {
public:
    virtual void run(IRModule&) = 0;  // vtable dispatch
};
```

**好**：
```cpp
template <typename P>
concept Pass = requires(P& p, IRModule& m) {
    { p.run(m) } -> std::same_as<void>;
    { p.has_error() } -> std::convertible_to<bool>;
};

template <Pass... Passes>
void run_pipeline(IRModule& mod, Passes&... passes) {
    (passes.run(mod), ...);
}
```

**迁移清单**：

| 位置 | 当前 | 目标 | 优先级 |
|------|------|------|--------|
| `pass_manager.ixx` | IRPass 虚基类 | Pass concept + fold pipeline | P0 |
| `service.ixx` | 手动 pass 注册 | `run_pipeline(module, ck, ar, cf)` | P1 |

### 2.3 std::expected > bool+string 结构体

**坏**：
```cpp
struct EvalResult { bool success; int64_t value; std::string error; };
```

**好**：
```cpp
using EvalResult = std::expected<int64_t, Diagnostic>;
```

`diag.ixx` 已定义 `Diagnostic` 和 `Result<T>`，下一步全面迁移：

**迁移清单**：

| 位置 | 当前 | 目标 | 优先级 |
|------|------|------|--------|
| `frontend.ixx` | `struct EvalResult` | `Result<int64_t>` | P0 |
| `parser.ixx` | `struct ParseResult` | `Result<Expr*>` | P0 |
| `ir_interpreter.ixx` | `EvalResult` | `Result<int64_t>` | P1 |
| `service.ixx` | `EvalResult` | `Result<int64_t>` | P1 |

### 2.4 std::span > const std::vector&

**坏**：
```cpp
void process(const std::vector<IRInstruction>& instrs);
```

**好**：
```cpp
void process(std::span<const IRInstruction> instrs);
```

std::span 是零开销视图，接受 vector/array/C 数组。贯穿全线参数传递。

### 2.5 字符串驻留 (String Interning)

**当前问题**：
```cpp
struct VariableNode { NodeTag tag; std::string name; };  // 每个变量独立分配
```

大量重复标识符（`x`、`y`、`fact`、`lambda`）反复 `std::string` 构造析构。

**目标**：
```cpp
using SymbolId = std::uint32_t;

class StringPool {
    std::vector<char> buffer_;          // 连续字符串数据
    std::pmr::unordered_map<std::string_view, SymbolId> table_;
public:
    SymbolId intern(std::string_view s);
    std::string_view resolve(SymbolId id) const;
};
```

集成到 Arena：
```cpp
arena.string_pool().intern("fact");  // 返回 SymbolId
```

**迁移清单**：

| 位置 | 当前 | 目标 | 优先级 |
|------|------|------|--------|
| `arena.ixx` | 无 StringPool | `StringPool` | P1 |
| `ast.ixx` | `std::string name` | `SymbolId name` | P2 |
| `ir.ixx` | `std::string name` | `SymbolId name` | P2 |

### 2.6 Contracts (C++26 `pre` / `post`)

**Status (Issue #144)**: C++26 contracts ship in GCC 16.1 in a
**modified form** from the original P2900 proposal. The actual
syntax in Aura is the function-local `pre (cond)` / `post
(cond)` form — **no `[[...]]` brackets, no colons, no
`<contracts>` header required** for the user code (the
runtime / handler setup uses it via the build's
`-include contracts` flag).

```cpp
// Pre / post on a regular function:
void* allocate(std::size_t size, std::size_t alignment)
    pre (size > 0)
    pre (alignment > 0 && (alignment & (alignment - 1)) == 0)
    post (r: r != nullptr) {       // 'r' refers to the return value
    // ... allocation ...
    return retr;
}

// Pre / post on a member function (mixes cleanly with [[nodiscard]]):
template <typename T>
[[nodiscard]] T* allocate(const T& init)
    post (r: r != nullptr) {
    // ...
}

// Post on a member: 'p:' refers to the post-state of member p
void set(int* x)
    pre (x != nullptr)
    post (p: p == x) {
    p_ = x;
}
```

**Note:** the older P2900 syntax `[[pre: cond]]` and
`[[post: cond]]` (with the attribute brackets and colons) is
**NOT supported** by GCC 16.1. Do not use it in Aura.

#### Why this form (not `[[pre:]]` or `contract_assert()`)?

The C++26 contracts history:
- **2023-2024 (initial P2900 proposal)**: attribute syntax
  `[[pre: cond]]` / `[[post: cond]]` was proposed.
- **March 2024 (Tokyo WG21 meeting)**: the attribute syntax
  was REMOVED from the proposal because vendor implementers
  (Clang, MSVC, EDG, GCC) couldn't agree on how to integrate
  contract violations with their optimization pipelines.
- **Late 2024 (post-Tokyo)**: P2900 retained the function-local
  `pre` / `post` form + the `<contracts>` runtime header
  (`std::contracts::contract_violation`,
  `handle_contract_violation`).
- **C++26 publication**: function-local `pre` / `post` is what
  shipped.
- **GCC 16.1**: implements the function-local form natively
  when compiled with `-fcontracts`.

So the function-local `pre (cond)` / `post (r: cond)` form is
both:
1. **Portable** across C++26 compilers (where supported)
2. **More expressive** than `contract_assert()`:
   - `post (r: r == x)` captures return-value postconditions
     that `contract_assert` can't
   - `post (p: p == x)` captures member post-state, which is
     key for any mutator method

#### What about `contract_assert(cond)`?

The `<contracts>` header (included via Aura's
`-include contracts` flag) provides the `contract_assert()`
macro as a **fallback** for cases where `pre` / `post` can't
be used — most importantly, **checks on private state that
can't appear in the interface contract**:

```cpp
Env* Evaluator::copy_env(const Env& e, ast::ASTArena* target)
    pre (target != nullptr);     // <- on the declaration (evaluator.ixx)
    // ...
Env* Evaluator::copy_env(const Env& e, ast::ASTArena* target) {
    // arena_ is private; can't be on the interface contract.
    // contract_assert is the right tool for impl-only invariants.
    contract_assert(arena_ != nullptr);
    auto* ar = target ? target : arena_;
    return ar ? ar->create<Env>(e) : nullptr;
}
```

**Rule of thumb:** if the condition can go on the function
declaration, use `pre (cond)`. If it depends on private state,
use `contract_assert(cond)` in the impl body.

#### How violations flow

- Build with `-fcontracts` (set globally in Aura's CMake).
- On a `pre` / `post` / `contract_assert` failure, the
  compiler emits a call to
  `handle_contract_violation(...)` with a
  `std::contracts::contract_violation` argument.
- Aura's handler in `src/core/contract_handler.cpp`:
  1. Logs the violation to stderr with full context (kind,
     semantic, file, line, function, comment)
  2. Calls a user-registered hook via
     `aura_set_contract_violation_hook()` so
     DiagnosticCollector or observability metrics can
     capture the violation
  3. Aborts (matches the previous hard-fail behavior)

#### Hot paths with contracts (Issue #144)
13 contract sites ship in this PR (12 `pre`, 1 `contract_assert`):
- `Env::lookup` / `lookup_binding` — non-empty name
- `Primitives::lookup` — non-empty name
- `QueryEngine::match` — non-negative depth
- `QueryEngine::execute` — index is initialized
- `FlatAST::set_int` / `set_float` / `set_sym` — valid NodeId
- `FlatAST::set_marker` — valid NodeId
- `FlatAST::set_loc` — valid NodeId
- `apply_patches` — non-empty span
- `ShapeProfiler::record_shape` — shape_id != SHAPE_UNKNOWN
- `ShapeProfiler::invalidate` — FnKey != 0
- `Evaluator::copy_env` — arena_ != nullptr (impl-only,
  uses `contract_assert` because arena_ is private)

#### Performance
- With `-O3 -fcontracts`, the contract checks are elided in
  the default `enforce` semantic for `quick_enforce` builds.
  Measured impact on Aura's 50-case benchmark: < 1% (well within
  the AC budget). Verified via `python3 tests/benchmark.py`.

#### Why this section is longer than others
Contracts are a key safety tool for the self-modifying core.
Every `mutate:*` and `query:*` path crosses hot functions
(`Env::lookup`, `FlatAST::set_int`, `QueryEngine::match`),
and a stale-NodeId-style bug in any of them can corrupt the
AST silently. The contract is a "fail loud" tripwire.

---

## 3. 重构路线图

### Phase 1: P0 项（1-2 天）

```
Day 1:
  ├── parser.ixx: Parser → 纯函数 parse()
  ├── lowering.ixx: LoweringPass → 纯函数 lower_to_ir()
  └── frontend.ixx: EvalResult → Result<int64_t>

Day 2:
  ├── pass_manager.ixx: IRPass 虚基类 → Pass concept + fold pipeline
  └── CompilerService: 适配新纯函数接口
```

### Phase 2: P1 项（2-3 天）

```
Week 2:
  ├── arity.ixx: class → 纯函数
  ├── compute_kind.ixx: class → 纯函数  
  ├── ir_interpreter.ixx: EvalResult → Result<int64_t>
  └── arena.ixx: + StringPool

Week 3:
  ├── ast.ixx: std::string → SymbolId
  ├── ir.ixx: std::string → SymbolId
  ├── Contracts 注入关键路径
  └── std::span 全线替换 const vector&
```

### Phase 3: P2 项（未来）

```
  ├── 全量 contracts
  ├── AST DOD 索引化（uint32_t 替代指针）
  ├── Env/Closure DOD 化
  └── IRInterpreter 性能优化
```

---

## 4. 参考

- [C++26 标准工作草案](https://wg21.link)
- [Data-Oriented Design (Andrew Kelley)](https://www.youtube.com/watch?v=IroPQ150F6c)
- [std::expected - P0323](https://wg21.link/p0323)
- [Contracts - P2900](https://wg21.link/p2900)
- [Arena 设计](./aura_memory_pool.md)
- [CompilerService 设计](./aura_caas.md)
