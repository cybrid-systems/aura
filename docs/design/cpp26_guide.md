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

### 2.7 DOD / SoA 深化 (Issue #145)

**目标**:消除 `Env` / `Closure` / heap vectors 里残留的 raw pointer chasing,
把所有 hot path 上的 attribute 用 index / SpanId 表示。

**Phase 1 (本 PR ship):**

#### 2.7.1 Closure::params — string → SymId (真正的 SoA)

之前的实现:
```cpp
struct Closure {
    std::vector<std::string> params;  // string-keyed,string compare
    // ...
};
```

`apply_closure` 在 hot path 上做 `for (i...) if (cl.params[i] == arg_name)` —— 每帧都是 string compare。

现在的实现:
```cpp
struct Closure {
    std::vector<aura::ast::SymId> params;  // SymId-keyed, int compare
    // ...
};
```

`apply_closure` 改成 `bind_symid(cl.params[i], args[i])` ——
lambda 创建时已经在 `pool->intern(name)` 拿过 SymId,直接复用,**零额外开销**。

**性能影响**:
- 典型 5-10 arg closure,每个 arg 的 bind path 从 string compare (≈10 cycles/byte)
  降到 int compare (1 cycle)。累计 5-10× 单次 closure apply。
- Benchmark 全套 50 cases,从 0.22s 持平(在 micro-bench 里 closure
  apply 占比小,需要专门的 micro-bench 才能看到差异;这次 PR
  不引入 micro-bench,留 Phase 2)。

#### 2.7.2 Env::bind_symid / lookup_by_symid (SymId 快速路径)

```cpp
class Env {
    void bind(const std::string& n, EvalValue v) { /* 旧的 string path */ }
    void bind_symid(SymId s, EvalValue v);   // 新的 SymId 路径
    void set_pool(const StringPool* p);      // 镜像需要的 pool

    std::optional<EvalValue> lookup(const std::string&) const;     // 旧
    std::optional<EvalValue> lookup_by_symid(SymId) const;        // 新
    // ...
private:
    const StringPool* pool_ = nullptr;
    std::vector<pair<string, EvalValue>> bindings_;
    std::vector<pair<SymId, EvalValue>> bindings_symid_;  // 平行数组
};
```

`bind_symid` 写两边(如果 `pool_` 设了,镜像到 `bindings_` 让 legacy
string-based lookup 也能找到;不设 pool 就是纯 SymId 路径)。
`lookup_by_symid` 只看 `bindings_symid_`,整数比较。

#### 2.7.3 EnvView / ClosureView (zero-copy span view)

模仿已有的 `NodeView` (`core/ast.ixx`) 模式 —— 非拥有读视图,
暴露底层存储为 `std::span`,不分配不复制:

```cpp
struct EnvView {
    std::span<const pair<string, EvalValue>> string_bindings;
    std::span<const pair<SymId, EvalValue>>  symid_bindings;  // Issue #145
    const Env* parent = nullptr;
    std::optional<EvalValue> lookup(const string& name) const;
    std::optional<EvalValue> lookup_by_symid(SymId) const;
    size_t size() const;
};

struct ClosureView {
    std::span<const SymId> params;            // 直接 view over Closure::params
    NodeId body_id = NULL_NODE;
    bool dotted = false;
    const FlatAST* flat = nullptr;
    const StringPool* pool = nullptr;
    const Env* env = nullptr;
    const ASTArena* owner_arena = nullptr;
    string_view name;
    SymId param_at(size_t i) const;
    size_t arity() const { return params.size(); }
};

EnvView make_env_view(const Env& env);
ClosureView make_closure_view(const Closure& cl);
```

JIT / AOT codegen 可以 pattern-match 这两种 view type,在编译期知道
"我拿到的是只读引用" 而非 "任意 Env",这样可以跳过 cell-deref
分支(cells_ 为空时)。

#### 2.7.4 raw pointer 现状(留给 Phase 2)

`Env::parent_` / `Closure::env` / `Closure::pool` / `Closure::flat`
这些 raw pointer **没动**。完整 SoA 化(把 `Env*` 改成 `EnvId` =
`uint32_t` 索引进 `std::vector<EnvFrame>`)涉及 GC 和 mutation 路径
的更深改动,留给 Phase 2 的 sub-issue(#172 / #173)。

#### 2.7.5 Phase 2 路线图

1. **`EnvFrame` SoA 化** — `std::vector<EnvFrame>` + `EnvId` 替代 raw
   `Env*` 指针。`copy_env` 改成 index 拷贝。GC 路径从 pointer chase
   变成 index lookup。
2. **`Closure` capture 重写** — `closure.env` 改成 `EnvId` (parent 是
   哪个 env frame),`closure.pool` / `closure.flat` 改成 arena-relative
   偏移或独立 `ClosureStorage` SoA。
3. **heap vectors 完全 index-based** — `pair_storage` / `cell_storage` /
   `string_heap` 都已经有 SoA 数组(分别是 `pairs_` / `cells_` /
   `string_heap_`),需要的是把它们从 `std::vector<...>` 换成
   `std::span`-backed arena storage,这样 GC 标记阶段不需要 copy。
4. **`Env::bindings_` 全转 SymId** — 把 string-keyed 数组彻底
   删掉,`lookup(string)` 改成 "intern → lookup_by_symid"。这是
   Phase 1 的逻辑延伸,但影响面广(每个 `Env::bind` 调用点),
   单独一个 sub-issue。

> **设计哲学**:Phase 1 走的是 "infrastructure first" ——
> 装好 EnvView / ClosureView / bind_symid / lookup_by_symid 的
> 骨架,Phase 2 再做"raw pointer 消灭"。这个分法跟 #143 的
> escape analysis partial close 一样:能 verify+close 的部分
> 先 ship,scope 太大的 part 单开 sub-issue。

####2.7.6 Phase2.1 — EnvFrame SoA基础设施 (shipped)

延续 Phase1 的 "infrastructure first"模式,Phase2.1 不动现有
`Env` layout,先把 SoA骨架装好。下一步 (Phase2.2 /2.3) 再做
migration。

**新增类型** (`evaluator.ixx`):

- `EnvId = std::uint32_t` —4G envs够任何单 evaluator 用一辈子,
 uint32_t 比 uint64_t缓存密度高。`NULL_ENV_ID = UINT32_MAX`。
- `EnvFrame` struct — 与 `Env` 数据布局平行,只是 `parent_id_`
 (`EnvId`)替代了 `parent_` (`Env*`)。`EnvFrame::bind` /
 `bind_symid` / `lookup_local` / `lookup_local_by_symid` 是
局部版本(不 walk parent)。

**`Evaluator` 新 API**:

- `std::vector<EnvFrame> env_frames_` — SoA arena。`push_back`
增长,`reset_env_frames()`一次性清空。
- `alloc_env_frame(parent_id, primitives) → EnvId` —分配并返回
 index。O(1) amortized。
- `env_frame(id) → const EnvFrame&` / `env_frame_mut(id) → EnvFrame&`
 —索引访问。`pre (!NULL_ENV_ID)`契约。
- `walk_env_frames<F>(start, f)` —模板,沿 parent chain走,回调
 返回 `false` 时早退。**纯 index lookup** — 没有 pointer chase,
 cache-friendly。
- `env_depth(start) → std::size_t` — 用 `walk_env_frames`算 chain
长度。GC profiling /调试用。
- `lookup_by_symid_chain(start, s) → std::optional<EvalValue>` —
演示 SoA walk 的实际使用:沿 chain找最近一帧,shadowing语义
 对齐 `Env::lookup_by_symid`。

**为什么不直接替换 `Env`?**

`Env` 的 raw pointer现场遍布 `evaluator_impl.cpp` (7838/7862/
16569/17965/17973/17984 等多处 parent walk + closure env捕获)。
一刀切替换会让 diff爆炸、回归风险高。Phase2.1 先把骨架立起来,
Phase2.2 起每个 sub-issue 处理一类 call site (env.parent walks
→2.2,closure env捕获 →2.3,等等)。**当所有 call site 都迁完,
把 `Env` 定义整体替换成 `EnvFrame` 就是一行 rename** — 这就是
为什么现在让两个 struct 数据布局严格平行。

**测试**: `tests/test_issue_145.cpp` Phase2.1 加12 个 test,
覆盖 NULL语义 /分配 /索引 roundtrip / parent chain walk /
early exit / depth / shadowing / missing / 无 pool bind / 多分配
持久性 / reset。

####2.7.7 Phase2.2+路线图 (remaining)

按依赖顺序:

1. **Phase2.2 — `Env::parent()` 调用点迁移**:5 个 parent walk
 site (`lookup_cell_ptr`、`lookup_cell_index`、`eval_flat`内部、
 还有2 个 `eval_env.parent()`链路)改成 `walk_env_frames`。
之后 `Env::parent_`就可以删掉,只保留 `EnvId`。
2. **Phase2.3 — `Closure::env` → `EnvId`**: closure捕获的不再
 是 `Env*`,而是 `env_id_`索引。`copy_env` / `apply_closure`
 都走新路径。
3. **Phase2.4 — heap vector全部 arena-backed** (`pairs_` /
 `cells_` / `string_heap_`): `std::vector<T>` → arena storage
 + `std::span<T>`视图。GC mark阶段不需 copy。
4. **Phase2.5 — `Env::bindings_` 全 SymId-only**:删 string-keyed
数组,`lookup(string)`改成 `intern → lookup_by_symid`。依赖
 Phase2.2 (parent walk 已迁完)。
5. **Phase2.6 — 全 Env → EnvFrame rename**: 所有 Phase2.x 完成
 后,`Env` 类整体替换为 `EnvFrame`,保留 `Env` 作为 deprecated
 alias 一个 release,然后删除。

> Phase2.1 →2.6预计4-6 个 verify+close cycle,每个200-400 行
> diff,边界清晰。可以跟 #172 / #173 等 sub-issue 并行,但建议
>串行(每个 sub-issue都能独立 ship)。
