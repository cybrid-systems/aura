# Aura Evaluator — Developer Guide

**Status:** Living document. Add patterns as they're discovered; keep examples
under 30 LOC where possible.

**Audience:** Anyone touching `src/compiler/evaluator_impl.cpp` or
`src/compiler/evaluator.ixx` — adding a primitive, fixing a cache
invalidation bug, or wiring a new thread-safety boundary.

**Origin:** Created for Issue #112 as the explicit follow-up to the
self-modifying-flat audit (#111). The audit's last paragraph was:

> This should be documented in the developer guide for the
> evaluator (in `docs/developer/evaluator.md` or similar — to be
> created).

This is that document.

---

## 0. Mental model

The Aura evaluator walks a **FlatAST** (a flat `std::pmr::vector<Node>`
indexed by `NodeId`) paired with a `StringPool`. The tree-walking
interpreter (`eval_flat` at line ~15857) is the reference implementation;
the IR interpreter (`IRInterpreter` in `ir_executor_impl.cpp`) is a
downstream consumer that must observe the same observable behavior.

Everything in this file is about making primitives correct under
this model. There are three invariants you cannot violate:

1. **Flat can grow during evaluation.** `parse_to_flat`, `set_child`,
   `add_mutation`, `add_node` all append to the same `FlatAST` that
   the primitive may be iterating.
2. **A query may run concurrent with a mutate.** Multiple agents,
   fibers, and the `--serve` REPL all share the same `Evaluator`
   instance. The `workspace_mtx_` shared/exclusive lock is the
   boundary.
3. **A change to one node invalidates the def-use index for the
   symbols it defines or uses.** The `defuse_touch_fn_` /
   `defuse_affected_syms_` protocol is the boundary.

If you're adding a new primitive and unsure which invariants apply,
assume all three and read §2–§4 below.

---

## 1. The self-modifying-flat iteration rule (Issue #111 lesson)

**This is the #1 footgun.** A `for (...; id < flat.size(); ++id)`
loop reads `flat.size()` on every iteration. If the loop body (or
anything the loop body calls transitively) modifies the flat, the
condition re-evaluates to `true` for the newly-appended indices and
the loop runs forever or until OOM.

**The audit in `docs/design/issue-111-audit.md` covered 22 such
loops in `evaluator_impl.cpp`. The `qar` primitive in `d25f066`
was the only one that triggered the bug. The fix is one line.**

### Rule: snapshot `flat.size()` before the loop

```cpp
// ❌ WRONG — will hang or OOM if the body can grow the flat
for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
    if (matches_predicate(flat, id)) {
        // ... or any call that transitively calls parse_to_flat / set_child
    }
}

// ✅ RIGHT — terminate after the original nodes
const auto end_id = flat.size();
for (aura::ast::NodeId id = 0; id < end_id; ++id) {
    if (matches_predicate(flat, id)) {
        // safe to call flat-mutating operations here
    }
}
```

### When the rule applies

Apply it whenever **all three** are true:

1. The loop iterates over a `FlatAST` (workspace flat OR a local
   flat that another part of your code might append to).
2. The loop condition re-reads `.size()` each iteration.
3. The loop body, or anything reachable from it, may append to
   that same flat via `parse_to_flat`, `set_child`, `add_mutation`,
   `add_node`, `mark_dirty_upward` (which can schedule a rebuild),
   or `string_heap_.push_back` on a `pmr::vector<string>` whose
   allocator backs the flat (the original `qar` failure mode).

If any of (1)–(3) is false, the original `id < flat.size()` form
is fine. Most read-only `query:*` loops are safe (see the audit
table for the 22 confirmed-safe cases).

### When in doubt

Add the `end_id` snapshot. It costs one local register and never
changes correct-program behavior. The cost of omitting it is an
infinite loop, which is much worse.

## 5. Documenting your changes (Living Docs)

This is a **living project**. When you add, modify, or remove a primitive (especially EDSL-related ones like query:*, mutate:*, workspace:*, ast:*), you **must** keep the documentation in sync.

### Mandatory updates checklist
1. **Update the relevant design doc's §0 Implementation Status table**:
   - Add the primitive to the C++ Core Layer table (with ✓ / source location in evaluator_impl.cpp).
   - Update Aura Layer if there's a std/ wrapper.
   - Refresh the date and "AI Agent 读者请注意" if behavior changes.
   - Example files: `design/core/query_edsl.md`, `mutate_api.md`, `workspace_layering.md`, `typed_mutation.md`.

2. **Update api-reference.md (the central Primitives Surface)**:
   - Add to the built-in lists under "代码自修改 (EDSL)" or "Workspace".
   - Update the "EDSL Primitives Surface" subsection with the new code location (e.g. `primitives_.add("foo:bar", ...)` in evaluator_impl.cpp ~Lxxxx).
   - Add or update the corresponding row in the std/ EDSL table if a helper is provided.

3. **Update cross-references**:
   - In the design doc, add a "Code References" subsection (or extend existing) pointing to exact lines in `evaluator_impl.cpp`, `query.ixx`, `service.ixx`, etc.
   - If it affects Agent usage or examples, touch `tutorial.md` status notes.

4. **For new speculative work**:
   - Do **not** create new files in `design/history/notes/` unless it is a truly unresolved exploration.
   - Route most work through a closing note + update to the core design §0.
   - See `design/history/README.md` and `design/history/notes/README.md` for rules.

5. **Status banners & dates**:
   - Keep the top-level `> **Status (日期, Issue #):**` or `## 0. Implementation Status` fresh.
   - Update the date on any meaningful change.

### Why this matters
- AI Agents (via --serve) rely on accurate "what works now" information.
- Human contributors need to find the authoritative spec + implementation mapping quickly.
- The §0 tables + api-ref Surface are the single source of truth after the Phase 1–3 cleanup.

Failing to update docs is equivalent to leaving a bug in the self-modification surface.

See the full "Living Documentation Practices" section in `docs/README.md` for the complete rules.

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

### 3.2 The `merr` helper

Every `mutate:*` primitive has the same error-construction boilerplate.
Define `merr` as a local lambda at the top of the lambda body:

```cpp
auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
    auto mi = string_heap_.size(); string_heap_.push_back(m);
    auto ki = string_heap_.size(); string_heap_.push_back(k);
    auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
    auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
    return kp;
};
```

This produces the canonical `("error" . ("kind" . "message"))` shape.
There are ~15 copies in the file; consolidating into a private
member is a future cleanup.

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
  slot is in the `g_adt_constructors` table, see §7).

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

## 7. ADT constructor table

`g_adt_constructors` (line 168) is a `std::unordered_map<string,
AdtCtorEntry>` that bypasses Aura's normal Begin-scoped-define
rule. The `(datatype ...)` parser special form registers
constructors here, so a `(Leaf 42)` call resolves the `Leaf` name
*globally* even though the `datatype` form is a single top-level
expression that returns `()`.

### Why this exists

`docs/design/issue-108-datatype-followup.md` documents the two
failed attempts. The conclusion: Aura's parser returns one root
node per top-level form, so `(datatype ...)` cannot emit N
top-level defines. The global ctor table is the workaround.

### When to add to it (not "how")

You generally don't add to it directly. The `parse_datatype` /
`AdtRegister` path does. But if you're adding a new "global
primitive-like" feature (e.g. a `register-handler` for some
event), the ctor table is the model: a global `unordered_map`,
a typed entry, and a 4th-priority lookup in `Env::lookup` (after
local, parent, primitives; see line 204).

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
- [ ] **§2.4 error returns**: matches the convention (tagged pair
      for mutate, `#f` for predicate, etc.)
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
| `src/compiler/evaluator_impl.cpp` | 18K lines: `init_pair_primitives`, `eval_flat`, all `query:*`/`mutate:*`/`ast:*`/`workspace:*` primitives, c-FFI, datatype |
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

Future sections to add as discovered:

- `dyn-*` primitive conventions (when added)
- IR lowering / `lower_to_ir` interaction
- jit bridge protocol
- aot / binary emit conventions
