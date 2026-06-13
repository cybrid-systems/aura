# Aura Evaluator 运行时核心开发者指南

**状态**：Living document（持续更新）。发现新模式就补充，例子尽量控制在 30 行以内。

**目标读者**：任何需要直接修改 `src/compiler/evaluator_impl.cpp` 或 `evaluator.ixx` 的人 —— 包括添加新 primitive、修复缓存失效、处理线程安全边界、接新调度 hook 等。

**为什么有这篇文档**：
Issue #111（self-modifying-flat 审计）之后发现：在 FlatAST 自修改 + 并发 + 多 session 的环境下写运行时代码极其危险。审计报告最后一段明确要求：

> This should be documented in the developer guide for the evaluator.

于是有了本指南。

---

## 这是什么？为什么叫 Evaluator？

在传统 Lisp 里，“evaluator” 就是负责执行代码的核心（`eval` 函数）。

在 Aura 里，`Evaluator` 类（定义在 `evaluator.ixx`，主要实现在 `evaluator_impl.cpp`）是**整个运行时的中枢**：

- 它持有当前 workspace 的 FlatAST + StringPool。
- 它管理所有内置原语表（`Primitives`），几乎所有语言扩展（包括 query:*、mutate:*、workspace:*、ast:* 等 EDSL 原语）都是通过 `primitives_.add(...)` 注册到这里的。
- 它负责真正的执行入口 `eval_flat`（树遍历解释器）。
- 它还维护了自修改所需的所有复杂不变式：workspace 锁、defuse 失效、mutation boundary yield、快照/回滚、增量脏标记等。

**一句话总结**：
Evaluator 不是普通的解释器，而是“让代码拥有自我意识”这个哲学在 C++ 层的具体承载者。它把自修改、并发安全、版本化 workspace 这些极高风险的能力集中管理起来。

普通 Aura 程序员（包括 LLM Agent）几乎不需要关心这里。他们通过 EDSL 和 stdlib 编程就够了。只有当你要**扩展运行时本身**（加新内置、修 evaluator 内部 bug、加新并发机制）时，才需要读这篇文档。

---

**Audience（精确范围）**：
- 直接动 `evaluator_impl.cpp` / `evaluator.ixx` 的人
- 需要理解 FlatAST 自修改安全规则的人
- 想加新 primitive 并且想一次写对的人

**不适合的人**：
- 只想用 Aura 写程序的人 → 去看 `tutorial.md` + `api-reference.md` + `design/core/`
- 只想理解高层设计的人 → 去看 `design/core/` 各文档的 `## 0. Implementation Status`

---

**写作原则**（本指南自己遵守）：
- 保持 Living document 性质
- 优先讲“不变式”和“为什么”，而不是“怎么实现某个功能”
- 例子短小，重点突出 footgun
- 所有新 primitive 相关改动，都必须同步更新本指南 + 对应 design/core/ 的 §0 状态表 + api-reference.md

以下内容就是围绕上面这些原则展开的。

---

## 0. Mental Model（核心模型）

Aura 的 Evaluator 操作的是一个 **FlatAST**（平坦的 `std::pmr::vector<Node>`，用 `NodeId` 索引）加上一个配套的 `StringPool`。

- 树遍历解释器 `eval_flat`（目前在 `evaluator_impl.cpp` 大约 15857 行附近）是参考实现，所有语义必须以它为准。
- IR 解释器（`IRInterpreter`）和 JIT 都是下游消费者，必须观察到完全一致的行为。

**本指南的所有内容，都是为了让 primitive 在这个模型下能正确、安全地工作。**

### 三个不可违反的不变式（Invariants）

1. **Flat 在求值过程中可以增长**  
   `parse_to_flat`、`set_child`、`add_mutation`、`add_node` 等操作都会往同一个 FlatAST 里追加节点。而 primitive 自己可能正在遍历这个 flat。

2. **query 和 mutate 可以并发发生**  
   多个 agent、多个 fiber、REPL、--serve 都可能共享同一个 Evaluator 实例。`workspace_mtx_` 的 shared/exclusive 锁是唯一边界。

3. **修改一个节点会让它定义/使用的符号的 def-use 信息失效**  
   必须通过 `defuse_touch_fn_` / `defuse_affected_syms_` 协议通知 DefUseIndex，否则后续查询会拿到陈旧结果。

**经验法则**：如果你要加一个新 primitive，又不确定要遵守哪几条，就**全部遵守**，然后仔细阅读后面章节。

---

## 1. 自修改 Flat 迭代铁律（Issue #111 的血泪教训）

**这是目前最大的 footgun。**

```cpp
for (aura::ast::NodeId id = 0; id < flat.size(); ++id) { ... }
```

只要循环体（或它调用的任何函数）可能往同一个 flat 里追加节点，`flat.size()` 每次都会变大，循环就可能永远跑不完，甚至 OOM。

Issue #111 审计了 `evaluator_impl.cpp` 里的 22 个类似循环。只有 `mutate:query-and-replace` 真正触发了 bug，但所有人都差点中招。

### 铁律：迭代前先 snapshot 大小

```cpp
// 错误示范
for (aura::ast::NodeId id = 0; id < flat.size(); ++id) { ... }

// 正确做法
const auto end_id = flat.size();
for (aura::ast::NodeId id = 0; id < end_id; ++id) { ... }
```

### 什么时候必须遵守？

同时满足以下三条就必须 snapshot：

1. 正在遍历一个 FlatAST（无论是 workspace 的还是局部的）。
2. 循环条件每次都会重新读 `.size()`。
3. 循环体或其可达代码可能通过 `parse_to_flat`、`set_child`、`add_mutation`、`add_node`、`mark_dirty_upward` 等往同一个 flat 追加内容。

如果以上任意一条不满足，原写法通常是安全的。大部分只读的 `query:*` 循环都是安全的（审计表里有 22 个确认安全的案例）。

**经验**：拿不准就直接 snapshot。少一个局部变量而已，省掉一个永远跑不完的循环，划算多了。

## 5. 改动必须同步的文档（Living Docs 要求）

这是一个活的项目。任何对 primitive 的增删改（尤其是 EDSL 相关的 query/mutate/workspace/ast），**都必须**同步更新文档。

### 强制同步清单
1. 更新对应 `design/core/` 文档的 `## 0. Implementation Status` 表格
   - 在 C++ Core Layer 表里加上新 primitive + evaluator_impl.cpp 位置
   - 如果有 std/ wrapper，也更新 Aura Layer 表
   - 更新日期和 “AI Agent 读者请注意” 段落

2. 更新 `docs/api-reference.md`（中央 Primitives Surface）
   - 在 EDSL / Workspace 列表里加上
   - 在 “EDSL Primitives Surface” 小节里写清楚代码位置

3. 在设计文档里加 “Code References” 小节
   - 指向 `evaluator_impl.cpp`、`query.ixx`、`service.ixx` 等具体位置

4.  speculative 探索不要乱建 notes/
   - 绝大部分工作应该走 core/ §0 更新 + closing
   - 只有真正未解决的愿景级设计才放 history/notes/

5. 保持状态横幅和日期新鲜
   - 每次有实质性行为变化都要更新日期

不更新文档就等于给自修改系统留下一个“隐形 bug”。AI Agent 依赖这些文档来判断“现在能用什么”。

完整规则见 `docs/README.md` 的 Living Documentation Practices 章节。

---

## 2. Adding a new primitive

Primitives are registered via `primitives_.add("name", lambda)` in
`init_pair_primitives()` (line 620) or in the `Evaluator()`
constructor (lines 8060+) for primitives that need `this`-capture.

### 2.1 The minimal primitive

```cpp
primitives_.add("my:primitive", [](std::span<const EvalValue> a) -> EvalValue {
    if (a.size() < 1 || !types::is_int(a[0]))
        return make_void();                 // or make_int(0) — see §2.4
    auto n = types::as_int(a[0]);
    return make_int(n * 2);
});
```

Three things to notice:

- **Capture `this` only if you need it.** Most primitives don't.
  Adding `[this]` means the lambda lives as long as the Evaluator,
  which is fine, but it forces registration in the constructor
  (where `this` is in scope) instead of `init_pair_primitives()`.
- **`std::span<const EvalValue>` is the only arg type.** Aura
  primitives are variadic. The `a.size()` check is mandatory.
- **Return an `EvalValue`, never throw.** Throw is reserved for
  unrecoverable internal errors. User-facing errors go through
  the value channel (see §2.4).

### 2.2 Capturing `this` — common reasons

| Need | Pattern |
|------|---------|
| Read `string_heap_`, `cells_`, `pairs_` | `[this]` + `string_heap_[...]`, `cells_[...]` |
| Mutate `workspace_flat_` | `[this]` + `*workspace_flat_` (lock first — §3) |
| Build / read `defuse_index_` | `[this]` + call `defuse_touch_fn_` (see §4) |
| Allocate in `arena_group_` | `[this]` + `arena_group().allocate<T>()` |
| Register a callback for closure dispatch | `[this]` + `set_*_fn_(...)` |

### 2.3 Construction helpers (from `value.ixx`)

The `EvalValue` API is in `src/compiler/value.ixx`. The
constructors you will use 99% of the time:

| Helper | Type | Notes |
|--------|------|-------|
| `make_int(int64_t)` | Int | most common return |
| `make_float(double)` | Float | coerce from int with `static_cast` |
| `make_bool(bool)` | Bool | not truthy/falsy; explicit |
| `make_string(idx)` | String | `idx` is into `string_heap_` (always heap) |
| `make_pair(idx)` | Pair | `idx` is into `pairs_` |
| `make_closure(id)` | Closure | id is `ClosureId`, see §5 |
| `make_primitive(slot)` | PrimitiveRef | passable as a value (Issue #62) |
| `make_void()` | Void | "no value" — different from `#<void>` |

Inspectors are the matching `is_*` / `as_*` pair:

```cpp
if (a.size() >= 2 && types::is_int(a[0]) && types::is_string(a[1])) {
    auto n = types::as_int(a[0]);
    auto s_idx = types::as_string_idx(a[1]);
    // ...
}
```

### 2.4 Error returns

Aura has no exception-based user-facing errors. Pick a convention
and stick to it:

| Convention | When | Example |
|------------|------|---------|
| Return `#f` | Predicate / option-style | `(set-empty? xs)` on a non-hash |
| Return `0` / `()` | Numeric / list ops | `(cdr 42)` → `()` |
| Return a tagged error pair | Mutate primitives (must report *why* it failed) | `("error" . ("bad-arg" . "usage: ..."))` |
| Return a Diagnostic via `EvalResult` | Internal eval failures only | recursion-depth, type errors from `eval_flat` |

Mutate primitives almost always use the tagged-pair pattern
(see `mutate:replace-type` at line 4180 for the canonical
`merr` lambda).

### 2.5 Argument validation checklist

Before you do anything else, validate:

- `a.size()` matches what you expect (variadic ⇒ loop; fixed ⇒ exact count)
- Each arg's `is_*` matches what the call site will pass
- For string args, `as_string_idx(a[i]) < string_heap_.size()`
- For node-id args, `static_cast<NodeId>(as_int(a[i])) < flat.size()`

The "index out of range" checks are easy to forget and crash
with no useful backtrace.

---

## 3. Mutate primitives — locking protocol

> **The full memory model is documented in
> [`docs/design/core/memory_model.md`](../design/core/memory_model.md)
> (Issue #157 Phase 4).** This section is the C++ maintainer's
> quick-reference; read the full doc for the JIT-layer protocol
> and the version-check fastpath exception.

`workspace_mtx_` is a `std::shared_mutex` (line 586 of
`evaluator.ixx`). The convention:

| Primitive kind | Lock | Why |
|----------------|------|-----|
| `mutate:*` that changes the workspace AST | `std::unique_lock` | exclusive — write |
| `query:*` (read-only AST walk) | `std::shared_lock` | shared — parallel reads OK |
| `query:index-stats`, `ast:version` (read metadata) | `std::shared_lock` | shared |
| `mutate:*` that only reads (e.g. `mutate:get` if it existed) | `std::shared_lock` | rare but legal |
| Anything that calls `ensure_defuse` from within | Don't hold the lock if you don't need to | `ensure_defuse` itself takes the lock |

### 3.1 Canonical mutate skeleton

```cpp
primitives_.add("mutate:foo", [this](std::span<const EvalValue> a) -> EvalValue {
    std::unique_lock<std::shared_mutex> wlock(workspace_mtx_);

    // 1. Validate args
    if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]))
        return merr("bad-arg", "usage: (mutate:foo node-id summary)");

    // 2. Bump defuse version
    defuse_version_++;

    // 3. Yield at mutation boundary (fiber scheduler integration)
    if (aura::messaging::g_fiber_yield_mutation_boundary)
        aura::messaging::g_fiber_yield_mutation_boundary();

    // 4. Validate workspace state
    if (!workspace_flat_) return merr("no-workspace", "...");
    auto& flat = *workspace_flat_;
    auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
    if (node >= flat.size()) return merr("out-of-range", "...");

    // 5. Apply mutation (use the §1 snapshot rule if iterating)
    const auto end_id = flat.size();
    for (aura::ast::NodeId id = 0; id < end_id; ++id) { ... }

    // 6. Mark dirty + record history
    flat.add_mutation_with_rollback(...);
    workspace_flat_->mark_dirty_upward(node);
    if (defuse_touch_fn_) defuse_touch_fn_(defuse_index_, sym);

    return make_int(mid);
});
```

### 3.2 The `make_merr` helper (centralized in refactor Step 0.1)

Every `mutate:*` primitive (and some query sites) needs to return a tagged
error pair of the shape `("error" . ("kind" . "message"))`.

**Use the centralized member** (added Step 0.1):

```cpp
return make_merr("bad-arg", "usage: ...");
// or inside a [this]-capturing lambda: return this->make_merr(...) or just make_merr
```

The implementation (in `evaluator_impl.cpp`) is exactly the body that used to be
duplicated as a local `auto merr = [this](...)` lambda in ~14 places.

See:
- Declaration: `evaluator.ixx` (private section, near other helpers).
- Definition: `evaluator_impl.cpp` (near `Primitives::Primitives()` / before `init_pair_primitives`).
- Call sites migrated starting in Step 0.2 (first the `mutate:replace-type` site that was the canonical example in older versions of this guide).

This consolidation was explicitly called out as a "future cleanup" before the refactor. Step 0.1 did the pure addition; 0.x/3.1 steps progressively remove the remaining local copies (query cluster ongoing).

### 3.3 The read-only fast path

`workspace_read_only_` (also on the Evaluator) is checked
**before** the lock acquisition in mutate primitives. When set,
mutate primitives return a `"read-only"` error without taking the
lock. This keeps the no-op fast path cheap. Do not skip this check
in your new mutate primitive — match the pattern in `mutate:replace-value`
and friends.

### 3.4 Don't re-enter the lock

`ensure_defuse`, `apply_closure` (IR bridge), and `typecheck-current`
each take the lock themselves. If your primitive holds the unique
lock and calls any of these, it will deadlock. The fix is to either:

- Defer the call to *after* releasing the unique lock, or
- Refactor so the primitive is split into "phase 1: validate
  under lock" and "phase 2: side effect without lock".

The 4 fuzzer paths flagged in #107 part 4 hit exactly this
problem (see the `Issue #107 part 4 (deferred)` comment near
line 588 of `evaluator.ixx`).

---

## 4. DefUseIndex touch protocol

The `DefUseIndex` (in `evaluator_impl.cpp` near line 7300) caches
def-use / reaches / effects query results. It has both a global
version (`global_version_`) and per-sym staleness (`stale_syms_`).

### 4.1 When a mutate primitive changes a node

You **must** do two things:

1. `defuse_affected_syms_.insert(name)` — fall back path (always
   works, even if `defuse_index_` is null).
2. `defuse_touch_fn_(defuse_index_, sym)` if `defuse_touch_fn_`
   is set — the fast-path that marks the sym stale inside the
   index without rebuilding it.

```cpp
defuse_affected_syms_.insert(name);
if (defuse_touch_fn_) defuse_touch_fn_(defuse_index_, sym);
```

### 4.2 Why both?

- `defuse_affected_syms_` is the authoritative list of "syms that
  need re-indexing on the next `ensure_defuse`". A future
  `mutate:*` path that forgets to call `defuse_touch_fn_` would
  silently leak stale data — so the list is the safety net.
- `defuse_touch_fn_` is the fast path. The callback is set by
  the DefUseIndex (forward-decl workaround: DefUseIndex is
  TU-local and the Evaluator header can't see its complete type).

### 4.3 When `defuse_touch_fn_` is null

That means the DefUseIndex hasn't been built yet. The
`defuse_affected_syms_` list still gets populated, so the next
`ensure_defuse` will rebuild from scratch and pick up the
mutation. Nothing to do — the protocol is null-safe.

---

## 5. Closures — when to use `make_closure` vs `make_primitive`

`make_closure(cid)` returns an `EvalValue` wrapping a
`ClosureId`. Lookups in the same evaluator find it in
`closures_`; cross-evaluator calls go through the `closure_bridge_`
callback (set by `CompilerService`).

`make_primitive(slot)` returns an `EvalValue` wrapping a
primitive slot index. Lookup goes through `primitives_->slot_for_name`.

### When to use which

- A lambda body that closes over local bindings → `make_closure`.
- A reference to a registered primitive (so it can be passed
  as a value, used in higher-order contexts) → `make_primitive`.
- A constructor function for an ADT → `make_primitive` (the
  slot is resolved via `adt_runtime_.find_ctor` (per-Evaluator, FFI pattern), see §7).

The `#62 PrimitiveRef` refactor unified these on the wire, but
the storage is still different (closures are heavier; primitives
are lightweight slot indices).

---

## 6. C FFI primitives

`c-load`, `c-func`, `c-call`, `c-close` are registered in the
`Evaluator()` constructor (line ~8060+). They use `dlopen` /
`dlsym` and global `g_ffi_libs` / `g_ffi_funcs` tables.

### Conventions

- **`c-load path` → `lib-id` (int)**. The lib id is the index
  into `g_ffi_libs`. Pass `-1` for `RTLD_DEFAULT` (no need for
  `c-load`).
- **`c-func lib-id "name" signature` → `func-id`**. Signature is
  the string form `"(ArgType) -> RetType"` (preferred) or the
  legacy int-form (return-type, then arg types).
- **`c-call func-id arg1 arg2 ...` → result**. Argument types
  must match the signature; mismatch logs an error and returns
  0.
- **`c-close lib-id` → 0/1**. Frees the lib handle.

Type system: `Int`, `Float`, `String`, `Opaque`, `Void`. Adding
a new type means editing `parse_ffi_sig` at line ~8090.

### Thread safety

`g_ffi_libs` and `g_ffi_funcs` are **not** locked. They're
append-only and the lib handle is immutable after `c-load`. If
you need to free mid-flight, take `heap_mutex()` first.

---

## 7. ADT constructor table (post-extraction, Step 2.3 / 5.1)

The ADT constructor table (supporting `(datatype ...)` forms that
must resolve constructors "globally" even from a single top-level
expression) was extracted in refactor Step 2.3 using the *exact*
FFI primitives pattern (RegisterFn callback to break cycles,
per-instance state, registration in Evaluator ctor).

**Current state (no more globals):**
- State lives in `AdtRuntime adt_runtime_;` (member of Evaluator, declared in `evaluator.ixx` parallel to `ffi_runtime_`; see adt_runtime.ixx + _impl.cpp).
- `AdtRuntime` owns the `unordered_map<string, AdtCtorEntry>` (ctor name → {name, arity, ...}).
- Registration of adt-related primitives (and ctor slots) is wired in the Evaluator ctor **after** the ffi one:
  ```cpp
  adt_runtime_.register_primitives(
      [this](std::string n, PrimFn f){ primitives_.add(std::move(n), std::move(f)); },
      &string_heap_);
  ```
- Lookups (Env::lookup fallback for constructors) now use:
  ```cpp
  if (auto slot = adt_runtime_.find_ctor(n)) return make_primitive(*slot);
  ```
- The observable bypass behavior is unchanged: `(Leaf 42)` still resolves even though the defining `(datatype ...)` produced only one root node.

`AdtRuntime` provides `register_primitives(RegisterFn, string_heap*)` and `find_ctor(const string&) const` (plus `ctor_count()` for tests). The implementation follows the ffi_primitives.ixx/_impl.cpp skeleton 1:1.

### Why the bypass still exists (historical)

`docs/design/issue-108-datatype-followup.md` (and 134-closing, 108-closing) document the two failed attempts at "normal" defines. Conclusion: parser returns one root node per top-level form, so `(datatype ...)` cannot emit N top-level defines. The (now per-Evaluator) ctor table + primitive bridge is the preserved workaround. See also parser_impl.cpp comments for the registration trigger path.

### When / how to extend

Extend inside `adt_runtime.*` (add metadata to AdtCtorEntry, move more parse/registration logic from parser/evaluator_impl into the register_primitives body following the stub). The public surface for callers remains the same (find_ctor + the registered adt:* primitives). Cross-reference: `CMakeLists.txt` (all adt entries annotated "ADT extraction (FFI pattern, hygiene 3.2)"), `docs/design/core/typesystem.md` §0, `mutate_api.md` §0, evaluator.md File map.

( Refactor 2.3/3.2/5.1 note: g_adt_constructors global + AdtCtorEntry struct + old registration block + direct Env lookups removed. All centralized to per-Evaluator adt_runtime_. No behavior change. )

---

## 8. Recursion guards

Two independent guards protect against runaway recursion:

### 8.1 `MAX_ENV_DEPTH = 1024` (line 129)

Guards `Env::lookup` against cyclic parent chains. `thread_local`
because lookup can be called from multiple fibers. Used as:

```cpp
if (++g_env_lookup_depth > MAX_ENV_DEPTH) {
    --g_env_lookup_depth;
    return std::nullopt;
}
struct _{ ~_() { --g_env_lookup_depth; } } dec;  // RAII decrement
```

### 8.2 `MAX_C_STACK_DEPTH = 2000` (line 15886)

Guards `eval_flat` against deep Aura recursion overflowing the
C++ call stack. `thread_local`. The fiber scheduler can run
eval on multiple threads, so the limit is per-thread:

```cpp
thread_local std::size_t t_c_stack_depth = 0;
struct DepthGuard {
    std::size_t& d;
    ~DepthGuard() { --d; }
} _dg{t_c_stack_depth};
if (++t_c_stack_depth > MAX_C_STACK_DEPTH)
    return std::unexpected(Diagnostic{...});
```

### When to adjust

Don't. If you hit one of these limits, you almost certainly have
a real infinite loop elsewhere. The right fix is the loop, not
the limit.

---

## 9. IRContext — bundling external state

`IRContext` (in `ir_executor.ixx` line 85) is a small struct
that bundles `Primitives&`, `TypeRegistry*`, and `CompilerMetrics*`
into one lifetime-tracked object. The `IRInterpreter` holds an
`IRContext&` instead of N separate references.

**Why this exists**: Issue #110 exposed a pre-existing
reference-invalidation bug pattern — `IRInterpreter` holding
`Primitives&` directly made it easy to construct an interpreter
in one scope and outlive the underlying `CompilerService`. The
`IRContext` doesn't fix the underlying issue (session management
in `main.cpp`), but it makes the lifetime explicit and visible
at the call site.

**Convention for adding new IR-runtime state**: extend `IRContext`,
not `IRInterpreter`. `escape_maps`, `EvalStrategy`, future
hook points — all belong in `IRContext`. The lifetime story is
"the context is stack-allocated at the call site, with lifetime
matching the interpreter's".

---

## 10. AST snapshot / restore

`ast:snapshot` and `ast:restore` (line ~9263) are the EDSL
versioning primitives. After #107 part 6, the snapshot is a
deep copy of the `FlatAST` + `StringPool` (heap-allocated via
`unique_ptr<FlatSnapshot>`). The restore is O(1) — just swap
the pointers.

### What the restore invalidates

```cpp
defuse_index_ = nullptr;  // or defuse_index_destroy(&defuse_index_)
defuse_affected_syms_.clear();
mark_all_defines_dirty_fn_();
pre_cache_workspace_defines_fn_();
```

The IR V2 cache is also reset (it keys on source-hash; the
restored flat produces different hashes).

### What the restore preserves

- **SymId identity** (the deep copy preserves string-pool order)
- **mutation_log_** (history survives restore)
- **type_id_** (no need to re-typecheck)
- **value_cache_** (eval-current cache hits across restore)

### The fallback path

If the direct deep copy throws (OOM), `ast:restore` falls through
to the source-based `set-code` path. This is slower and lossy
(sym ids may differ) but the snapshot at least exists.

---

## 11. Common pitfalls checklist

Before merging a new primitive, walk this list:

- [ ] **§1 snapshot rule**: any `for (...; id < flat.size(); ...)`
      loop with a body that can grow the flat — snapshot `end_id`
- [ ] **§3 locking**: mutate primitive takes `unique_lock`; query
      takes `shared_lock`; no re-entrant lock acquisitions
- [ ] **§4 defuse touch**: changing a node that defines/uses a
      sym calls both `defuse_affected_syms_.insert(name)` and
      `defuse_touch_fn_(...)` if set
- [ ] **§2.5 arg validation**: `a.size()` + `is_*` checks before
      any `as_*` or index access
- [x] **§2.4 / §3.2 error returns**: centralized `make_merr(k, m)` 100% COMPLETE (all ~14-15 original local lambdas eliminated across query+mutate; final one in extract-function removed). See make_merr in evaluator_impl.cpp + 0.1+3.1 steps. No more auto merr lambdas.
- [ ] **§5 closure vs primitive**: correct constructor for the
      use case
- [ ] **§3.3 read-only fast path**: if a mutate primitive, checks
      `workspace_read_only_` before lock
- [ ] **fiber yield at mutation boundary**: `g_fiber_yield_mutation_boundary()`
      after taking the lock, before the mutation
- [ ] **mutation history**: `add_mutation_with_rollback(...)` (or
      `add_mutation`) called so `ast:rollback` works
- [ ] **mark dirty**: `workspace_flat_->mark_dirty_upward(node)`
      for any node change
- [ ] **ASAN clean**: no leaks on snapshot+mutate+restore loop
- [ ] **fuzzers pass**: `fuzz_defuse --quick`, `fuzz_workspace
      --quick`, `fuzz_snapshot --quick` all at expected pass rates

---

## 12. Testing conventions

### Unit-level

For a new primitive `foo`, the minimum coverage is in
`tests/suite/core.aura` (or a dedicated file):

**3.2 pilot dedup note**: Common harness header (`tests/issue_test_harness.hpp`) introduced; the 3 CMake pilots now use `#include` to shrink boilerplate. Full adoption/thinning in later dedup steps. See pilots + roadmap 3.2.

**Phase 2 pilot (post 5.2)**: `aura_add_issue_test(NAME)` helper added to CMakeLists.txt; the 3 pilots are now single-line calls (dedup of the add_executable/set_property/compile/link/add_test repetition). This safely prototypes the macro before the 40+ full test_issue blocks. The helper + header together address the "extreme duplication" called out in the plan.

**Phase 2 pilot-2**: helper extended with base common core modules/impls (arena/ast/type etc.) + full standard link (pthread + stdc++ + aura-reflect); first real test_issue_132 (ast_walkers extraction verification) converted — block now ~3 lines (call + 1 specific append) instead of 15+ repeated lines. See CMakeLists.txt (helper + test_issue_132 block) + plan Phase 2. Follow-on small steps will convert more (generalize base or add parameters as needed).

**Phase 2 pilot-3** (infra): helper definition moved early in CMakeLists.txt (before any test_issue_* calls, near line 264) so all subsequent conversions are safe regardless of source order. (116 conversion was experimental and reverted to keep module closure simple during base stabilization.)

**Phase 2 pilot-4**: Converted test_issue_134 (ADT (datatype ...) verification, ties back to our 2.3/3.2 ADT extraction). Uses early helper + include extra (src/compiler) + append for parser/serve/type_checker/ir/coercion/observability bits. Block shrank dramatically. Base remains conservative (core + value/diag/value_impl). See CMakeLists.txt (134 block + early helper) + plan Phase 2.

**Phase 2 pilot-5**: Converted test_issue_135 (true parallel multi-agent orchestration verification). Heavy test (serve, parser, full evaluator*, JIT, LLVM, query, lowering, etc.). Uses early helper + appends for the long tail of sources + LLVM bits + rtti flags. Pattern holding: one (complex) test per small step. See CMakeLists.txt (135 block) + plan Phase 2.

**Phase 2 pilot-6**: Converted test_issue_136 (AOT name-mangler + memory/AOT/benchmark verification). Lightweight (headers + src/compiler include). Base + early helper makes it 4 lines. See CMakeLists.txt (136 block) + plan Phase 2. Continuing one-at-a-time dedup.

**Phase 2 pilot-7**: Converted test_issue_137 (full hygienic macros end-to-end + mutate compatibility + regression). Heavy (parser, serve, evaluator*, JIT, LLVM). Uses early helper + appends for the tail + rtti/LLVM bits. See CMakeLists.txt (137 block) + plan Phase 2.

**Phase 2 pilot-8**: Converted test_issue_158 (quasiquote should expand inner macro calls in code positions). Uses early helper + appends for parser/serve/evaluator/JIT/LLVM specifics. See CMakeLists.txt (158 block) + plan Phase 2. Continuing the one-at-a-time dedup.

**Phase 2 pilot-9**: Converted test_issue_159 (typecheck-incremental primitive for partial re-inference). Uses early helper + appends for parser/serve/evaluator/JIT/LLVM specifics. See CMakeLists.txt (159 block) + plan Phase 2.

**Phase 2 pilot-10**: Converted test_issue_161 (parser is a pure function; minimal subset, no evaluator/IR/JIT). Lightweight. See CMakeLists.txt (161 block) + plan Phase 2. Continuing one-at-a-time dedup.

**Phase 2 pilot-11**: Converted test_issue_162 (Type Concepts for the type system: TypeConstraint concept + concrete constraints + solve_constraints). See CMakeLists.txt (162 block) + plan Phase 2.

**Phase 2 pilot-12**: Converted test_issue_163 (expanded Pass concept usage: AnalysisPass, run_pipeline, run_analysis_pipeline, mark_coercions). See CMakeLists.txt (163 block) + plan Phase 2. Continuing one-at-a-time dedup.

**Phase 2 pilot-13**: Converted test_issue_164 (fiber:join spin-fallback elimination: 5 acceptance criteria for join, no-spin, concurrent mutate, parallel, perf). See CMakeLists.txt (164 block) + plan Phase 2.

**Phase 2 pilot-14**: Converted test_issue_165 (macro re-expansion + SyntaxMarker handling after EDSL mutations; TDD test, expected baseline FAIL on main). See CMakeLists.txt (165 block) + plan Phase 2.

**Phase 2 pilot-15**: Converted test_issue_166 (multi-layer cache invalidation Phase 1: epoch counter). See CMakeLists.txt (166 block) + plan Phase 2.

**Phase 2 pilot-16**: Converted test_issue_167 (IR layer SoA/DOD migration Phase 1: scaffold for IRModuleV2 + IRInstructionView API). Lightweight. See CMakeLists.txt (167 block) + plan Phase 2.

**Phase 2 pilot-17**: Converted test_issue_168 (incremental type cache safety Phase 1: epoch gate for set_cache_epoch API + basic flow with mutation). See CMakeLists.txt (168 block) + plan Phase 2.

**Phase 2 pilot-18**: Converted test_issue_170 (LLVM JIT AOT entry points Phase 1: verifies new public API on empty state). Lightweight. See CMakeLists.txt (170 block) + plan Phase 2. Continuing one-at-a-time dedup.

**Phase 2 pilot-19**: Converted test_issue_138 (incremental dirty propagation and fine-grained type checking for EDSL mutations: dirty-bit tracking, mark_dirty_upward, incremental cache, stress, perf). See CMakeLists.txt (138 block) + plan Phase 2.

**Phase 2 pilot-20**: Converted test_issue_139 (structural refactor primitives: extract-function, inline-call, rename-symbol, refactor/extract, move-node, splice, wrap; end-to-end on real Aura code). See CMakeLists.txt (139 block) + plan Phase 2.

**Phase 2 pilot-21**: Converted test_issue_140 (query:pattern EDSL primitive with Ellipsis and basic hygiene; hygiene fix for SyntaxMarker::MacroIntroduced). See CMakeLists.txt (140 block) + plan Phase 2.

**Phase 2 pilot-22**: Converted test_issue_141 (full WorkspaceTree with COW, read-only permissions, and merge primitives; all 18 workspace:* primitives end-to-end). See CMakeLists.txt (141 block) + plan Phase 2.

**Phase 2 pilot-23**: Converted (prep) test_issue_142 (composite query:where/filter + mutate:replace-subtree with capture/hygiene/rollback; 11 AC tests). Heavy (full serve/eval/JIT/LLVM + observability). Short form attempted; block restored full for build health (heavy pattern). See CMakeLists.txt (142 block) + plan Phase 2. Continuing the chain of similar one-at-a-time dedup ("这些类似的就一直做下去").

**Phase 2 pilot-24**: Converted test_issue_143 (escape analysis integration into pass_manager; IRFunction hand-crafted tests for return/call/MakePair/capture/pure/fixpoint ACs). Heavy (serve+eval+JIT+LLVM+observ). Short form landed cleanly (21/21). See CMakeLists.txt (143 block) + plan Phase 2.

**Phase 2 pilot-25**: Converted test_issue_144 (C++26 contracts integration: 13 contract_assert sites in hot paths + hook registration with DiagnosticCollector + runtime ACs exercising Env/Primitives/QueryEngine/FlatAST/apply_patches/ShapeProfiler). Heavy (serve + jit + llvm + contract_handler + shape). Short landed (12/12). See CMakeLists.txt (144 block) + plan Phase 2.

**Phase 2 pilot-26**: Converted test_issue_145 (DOD/SoA Phase 1 partial: Closure::params SymId migration, EnvView/ClosureView, bind_symid/lookup_by_symid fast path, 8 ACs + bulk env_frames stress). Consolidated split config. 3081/3081. See CMakeLists.txt (145 block) + plan Phase 2.

**Phase 2 pilot-27**: Convert test_issue_146 (pure-function + monadic Result extraction Phase 1: evaluator_pure module, coerce_to_int_pure as free Result, legacy wrapper compat, 4 ACs). Uses new pure import. Heavy (jit etc.). See plan Phase 2.

```scheme
; happy path
(display (foo 42))            ; expect 84
; error path
(display (foo "not-a-number")) ; expect ()
; arity path
(display (foo))                ; expect ()
```

### Fuzz-level

The three fuzzer scripts are the regression net:

- `tests/fuzz_defuse.py --quick` — def-use chain under heavy mutation
- `tests/fuzz_workspace.py --quick` — workspace state after many
  set-code / mutate cycles
- `tests/fuzz_snapshot.py --quick` — snapshot/restore round-trips

If your primitive touches the flat, all three should still pass
at their pre-existing pass rates (200/200, 290+/290+, 405/405
on main).

### ASAN

Run the test_ir suites under ASAN before merging anything that
allocates in a new code path:

```bash
cmake --build build --target test_ir -j
ASAN_OPTIONS=detect_leaks=1 ./build/test_ir
```

A new leak in your code shows up as a 4-6KB alloc-per-call. Fix
it before the regression lands.

---

## 13. File map

| File | What lives here |
|------|-----------------|
| `src/compiler/evaluator.ixx` | Evaluator class declaration, `workspace_mtx_`, callback hook setters |
| `src/compiler/evaluator_impl.cpp` | (being reduced via extractions) `init_pair_primitives`, `eval_flat`, query/mutate/ast/workspace primitives, ... (ADT moved to adt_runtime; all local merr lambdas centralized to make_merr in 0.1+3.1) |
| `src/compiler/adt_runtime.ixx` | AdtRuntime class + register_primitives (FFI pattern extraction complete); replaces old global g_adt_constructors |
| `src/compiler/adt_runtime_impl.cpp` | Impl for register + find_ctor (FFI pattern; extraction complete) |
| `src/compiler/value.ixx` | `EvalValue` POD + `make_*` / `is_*` / `as_*` helpers |
| `src/compiler/ir_executor.ixx` | `IRContext` struct + `IRInterpreter` class declaration |
| `src/compiler/ir_executor_impl.cpp` | `IRInterpreter::execute`, `run_function` (the actual opcodes) |
| `src/compiler/cache.ixx` / `cache_impl.cpp` | EDSL V2 IR cache (source-hash keyed) |
| `src/compiler/parser/*.cpp` | Parser special forms (e.g. `parse_datatype`) |
| `src/compiler/type_checker.ixx` | Type-check helper (used by `typecheck-current`) |

---

## 14. Related docs

- `docs/design/issue-110-followup.md` — qar crash root cause
  (the §1 rule's origin story)
- `docs/design/issue-111-audit.md` — 22-loop audit that
  recommended this guide
- `docs/design/issue-108-datatype-followup.md` — why the ADT
  ctor table exists
- `docs/design/defuse_analysis.md` — DefUseIndex data structures
- `docs/design/concurrency_model.md` — fiber scheduler
  integration (`g_fiber_yield_mutation_boundary`)
- `docs/design/cpp26_guide.md` — coding style targets (separate
  concern, but read together)

---

## 15. Versioning

- v1 (2026-06-07, Issue #112): initial creation. Covers §1
  self-modifying-flat rule, §3 locking, §4 defuse touch, §5
  closures, §6 FFI, §7 ADT, §8 recursion guards, §9 IRContext,
  §10 snapshot/restore.
- 5.1 (2026-06-12, plan): small living-docs sync — §5/§7 ADT section fully modernized post-extraction (g_adt_constructors → AdtRuntime per-Evaluator + FFI); typed_mutation.md + adt_* comments polished; checklist already marked 100% merr complete; File map accurate.
- 5.2 (2026-06-12, plan): tiny hygiene — adt_runtime_.register_primitives now accepts (and call site passes) the full set of pointers (string_heap + opaque_heap + coverage_counters) exactly as ffi_runtime_ for cross-extraction consistency. No behavior change (params (void)ed in stub). See ctor ~9042 and adt_runtime.ixx.

Future sections to add as discovered:

- `dyn-*` primitive conventions (when added)
- IR lowering / `lower_to_ir` interaction
- jit bridge protocol
- aot / binary emit conventions
