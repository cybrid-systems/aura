# Issue #131 — Refactor: extract FFI primitives from monolithic evaluator_impl.cpp

## Status: 🟡 Partial (FFI primitives extracted; 8 other planned modules from the issue are deferred as follow-ups)

The issue describes a major refactor: split
evaluator_impl.cpp (18,729 lines, not 1,248 as the issue
states) into 8+ focused modules. This PR ships the
first extraction: the C FFI primitives.

## What changed

### 1. New module: `src/compiler/ffi_primitives.ixx`

Exports `FFIRuntime` — a class that owns the FFI state
(loaded libraries, registered C functions) and provides
a `register_primitives(RegisterFn, ...)` method to wire
the FFI primitives (c-load, c-func, c-opaque, c-alloc,
c-free, c-struct-*) into a `Primitives` table via a
callback.

The `parse_ffi_sig` helper is inlined in the .ixx so
tests can use it without linking `ffi_primitives_impl.cpp`
(which transitively depends on libaura-reflect symbols).

### 2. Implementation: `src/compiler/ffi_primitives_impl.cpp`

Holds the FFI primitive lambda bodies (c-load, c-func,
c-opaque, c-alloc, c-free, c-struct-size, c-struct-set!,
c-struct-ref) — 8 primitives total. Each takes the
string_heap, opaque_heap, and coverage_counters as
borrowed pointers.

### 3. Evaluator integration

`src/compiler/evaluator.ixx`:
- Imports `aura.compiler.ffi_primitives`
- Adds `FFIRuntime ffi_runtime_` member

`src/compiler/evaluator_impl.cpp`:
- Removes `static std::vector<void*> g_ffi_libs;`,
  `struct FFIFunc`, `static std::vector<FFIFunc> g_ffi_funcs`
  (the file-scope statics)
- Removes the inline FFI primitive lambdas in the ctor
- Replaces with a single call:
  `ffi_runtime_.register_primitives(callback, &string_heap_, &opaque_heap_, &coverage_counters_);`
- Updates the apply_closure lookup site to use
  `ffi_runtime_.func_count()` and `ffi_runtime_.func_at(i)`
  instead of `g_ffi_funcs.size()` and `g_ffi_funcs[i]`

### 4. Regression tests (18/18 pass)

`tests/test_issue_131.cpp` exercises the new
`FFIRuntime` class in isolation. Coverage:
- **FFIRuntime standalone** (2 tests): fresh instance
  has 0 libs and 0 funcs.
- **parse_ffi_sig valid** (10 tests): handles Int,
  Float, String, Opaque, Void types; multi-arg
  signatures; whitespace.
- **parse_ffi_sig invalid** (6 tests): missing `->`,
  missing `(`, unknown type.

## Why the new design works

### Why a per-instance FFIRuntime instead of file-scope statics

The previous `g_ffi_libs` and `g_ffi_funcs` were
file-scope statics — shared across ALL evaluators in
the process. This was a global mutation hazard:
creating a second Evaluator would corrupt the first
one's FFI state. The new `FFIRuntime` instance is a
member of `Evaluator`, so each Evaluator has its own
FFI state. The issue's original code even noted:
"Global FFI state (shared across all evaluator
instances). In production, this should be
per-evaluator." — done.

### Why a RegisterFn callback instead of a Primitives&

The naive approach is to pass `Primitives&` to
`FFIRuntime::register_primitives`. But that creates a
cyclic import: `evaluator.ixx` defines `Primitives` and
imports `ffi_primitives.ixx` (for the `FFIRuntime`
member), and `ffi_primitives.ixx` would need to
import `evaluator.ixx` (for `Primitives`).

The fix: accept a `std::function<void(string, PrimFn)>`
callback. The caller (Evaluator's ctor) provides a
lambda that calls `primitives_.add(name, fn)`. The
FFIRuntime doesn't need to know about `Primitives` at
all — just the callback signature. This breaks the
cycle.

The cost: one extra std::function allocation per FFI
primitive registration (in the ctor, not in the hot
path). Acceptable.

### Why inline `parse_ffi_sig` in the .ixx

`parse_ffi_sig` is a pure function — no I/O, no global
state. It belongs in the header. The alternative
(keeping it in the .cpp) means tests that want to use
it must link the entire ffi_primitives_impl.cpp, which
transitively depends on libaura-reflect symbols
(`aura_alloc_float`, `aura_float_ref`). By inlining it
in the .ixx, the test can use it without those
linker pulls. Same pattern as the other pure helpers
in the codebase.

## Known limitations (out of scope for #131)

The issue proposed splitting evaluator_impl.cpp into
8+ focused modules:
- `primitives_core.ixx/.cpp` — basic arithmetic, lists,
  strings, IO primitives
- `query_primitives.ixx/.cpp` + `mutate_primitives.ixx/.cpp`
- `workspace_layering.ixx/.cpp` — COW, lock, merge
- `synthesis_strategies.ixx/.cpp`
- `ffi_primitives.ixx/.cpp` — **DONE (this PR)**
- `system_primitives.ixx/.cpp` — HTTP, getenv, regex,
  git
- `gc_variants.ixx/.cpp` + `intend_runtime.ixx/.cpp`

This PR ships only the FFI module. The others are
deferred as follow-ups. Each will follow the same
pattern: extract the focused code, replace inline
registration with a module call, keep behavioral
compatibility via the existing test suite.

### Other limitations
- The current `register_primitives` takes raw pointers
  to `string_heap_` and `opaque_heap_`. A future issue
  could use `std::span` for zero-overhead views (Issue
  #128 follow-up).
- The `FFIFunc` struct is in the public interface of
  `FFIRuntime` (for `func_at()`). A future issue
  could move it to a detail namespace.

## Acceptance criteria (this PR)

- `evaluator_impl.cpp` line count reduced: 18,729 → 18,475
  (264 lines extracted = 1.4% of the file).
- All 8 FFI primitives still work (no behavioral
  regression).
- Per-evaluator FFI state (no global statics).
- All existing tests pass (integ 148/148, typecheck
  10/10, 16 per-issue tests).

## Test status

- `integ`: 148/148 ✓
- `typecheck`: 10/10 ✓
- `test_issue_115..131` all 16 pass ✓

## What (if anything) is still open

- Extract the other 7+ planned modules (primitives_core,
  query/mutate, workspace_layering, synthesis_strategies,
  system_primitives, gc_variants, intend_runtime)
- Verify the per-evaluator FFI state doesn't leak
  (each Evaluator should free its own libs at destruction)
- Wire `FFIRuntime` into the observability snapshot
  (FFI lib count, func count)
- Use `std::span` for the string_heap/opaque_heap
  parameters (Issue #128 follow-up)
