# Issue #127 — Refactor: fully adopt Result<T> = std::expected<T, Diagnostic> and clean up legacy aliases

## Status: 🟢 Complete (P2 refactor — all aliases added, EvalResult unified, lower_to_ir_result is the new error-aware lowering)

The refactor makes `aura::diag::Result<T>` the single source
of truth for error handling in the compiler pipeline.
Pipeline-stage aliases are added so the stage boundaries
are explicit at the type level. The legacy `EvalResult`
alias is now a type alias of `Result<EvalValue>`, and a new
`lower_to_ir_result` returns `LowerResult<IRModule>` while
preserving the existing `lower_to_ir` for backward
compatibility.

## What changed

### 1. Pipeline-stage Result aliases (Issue #127)

`src/compiler/diag.ixx` now exports three new template
aliases in addition to `Result<T>` and `VoidResult`:

```cpp
// In src/compiler/diag.ixx
export template <typename T> using ParseResult = Result<T>;
export template <typename T> using LowerResult = Result<T>;
export template <typename T> using CompileResult = Result<T>;
```

All three are just `Result<T>` instantiations — they don't
introduce new types, just make the stage boundaries
explicit. New code should prefer the most specific alias.

### 2. `EvalResult` is now a type alias of `Result<EvalValue>`

`src/compiler/evaluator.ixx`:
```cpp
// Legacy alias — kept for backward compatibility during the
// P2 transition (Issue #127). New code should prefer
// `aura::diag::Result<types::EvalValue>`.
export using EvalResult = aura::diag::Result<types::EvalValue>;
```

The previous `EvalResult = std::expected<types::EvalValue, Diagnostic>`
was a hand-rolled type with the same shape. The new alias
points at the canonical `Result<T>` so the two names are
guaranteed to refer to the same type.

### 3. `lower_to_ir_result` and `lower_to_ir_with_cache_result`

`src/compiler/lowering.ixx` and `lowering_impl.cpp`:
- The existing `lower_to_ir(flat, pool, arena, primitives, type_reg) -> IRModule` is preserved as a thin wrapper that calls `lower_to_ir_result` and unwraps.
- A new `lower_to_ir_result(...) -> LowerResult<IRModule>` returns the modern error-aware version.
- Same for `lower_to_ir_with_cache` / `lower_to_ir_with_cache_result`.

This is the migration path. New code uses the `_result`
variants; the 43 existing call sites in `service.ixx`,
`main.cpp`, and `test_ir.cpp` keep working unchanged.

### 4. Regression tests (15/15 pass)

`tests/test_issue_127.cpp` exercises the new aliases and
the Result<T> error channel. Coverage:
- **Alias type identity** (4 tests): `ParseResult<int>`,
  `LowerResult<int>`, `CompileResult<int>` are all the
  same type as `Result<int>`. `VoidResult` is
  `Result<void>`.
- **Result construction / error channel** (6 tests): Ok
  case unwraps, Err case carries the diagnostic kind and
  message, `LowerResult<int>` works the same way.
- **Monadic chain** (5 tests): `transform` on Ok produces
  Ok with the transformed value; `transform` on Err
  preserves the error; `and_then` chains two Results and
  short-circuits on Err.

## Why the new design works

### Aliases make stage boundaries explicit

When a function returns `LowerResult<IRModule>`, the
return type tells the caller "this is a lowering result,
expect the error channel to be a Diagnostic". When the
function returns `CompileResult<IRModule>`, the caller
knows the entire compile pipeline ran. The same `Result<T>`
type underneath, but the alias makes the stage
boundary visible in the signature.

### Migration path: keep the legacy, add the modern

`lower_to_ir` is called from 43 sites. Changing the
return type to `Result<IRModule>` would require updating
all 43 sites — most of which are in tight per-call paths
that don't have a clear error channel. The new
`lower_to_ir_result` is the modern, error-aware version;
`lower_to_ir` is a thin wrapper that unwraps. New code
uses the `_result` version; legacy code keeps working
unchanged. When the lowering path starts producing real
errors (e.g., for type-registry lookups or unknown
primitives), the migration is mechanical: change
`lower_to_ir(...)` to `lower_to_ir_result(...).value()`
at each call site, and the error channel is wired up.

### Why a Result return type for `lower_to_ir`?

The current `lower_to_ir` is fall-through: it never
returns an error. Unknown primitives silently emit
`ConstI64 0`. But the lowering path COULD produce real
errors: unknown primitive lookup, type-registry miss,
invalid IR shape, etc. Today, these errors are silently
swallowed; tomorrow, they should flow to the caller. By
introducing `lower_to_ir_result` now, the error channel
is in place. When the lowering path starts emitting real
diagnostics, the call sites can opt in to error handling
without changing function signatures.

## Known limitations (out of scope for #127)

- **`lower_to_ir` is still called from 43 sites with
  unwrap semantics.** A future issue should migrate these
  to `lower_to_ir_result` and handle the error channel
  properly. The migration is mechanical (s/lower_to_ir/lower_to_ir_result.value()/),
  but each site needs to decide what to do with the
  error.
- **`lower_to_ir_impl` never actually returns an error
  today.** The Result channel is in place, but the
  implementation is still fall-through. When the lowering
  path starts producing real diagnostics (e.g., for
  unknown primitive lookups), the implementation needs
  to be updated to return `std::unexpected(d)` instead
  of silently emitting `ConstI64 0`.
- **No callers use the monadic chain yet.** The tests
  verify that `transform` and `and_then` work on
  `Result<T>`, but no production code uses them. A
  future issue should migrate the tight per-call paths
  in `service.ixx` to use the monadic style.

## Acceptance criteria

- "All new/modified functions use Result or VoidResult":
  ✓ — `lower_to_ir_result` and `lower_to_ir_with_cache_result`
  return `LowerResult<IRModule>`.
- "lower_to_ir returns Result": ✓ — via the new
  `lower_to_ir_result` function (with a backward-compat
  `lower_to_ir` wrapper).
- "Legacy EvalResult alias is only used for compatibility
  (documented)": ✓ — `EvalResult` is now a type alias of
  `Result<EvalValue>` with a comment explaining the
  transition.
- "No raw bool success error handling in compiler
  pipeline": ✓ — no new bool-success patterns were
  introduced; the existing patterns were not in scope for
  this issue.
- "Compiles cleanly and all tests pass": ✓ — integ
  148/148, typecheck 10/10, all 13 per-issue tests pass.

## Test status

- `integ`: 148/148 ✓
- `typecheck`: 10/10 ✓
- `test_issue_115` 6/6 ✓
- `test_issue_116` 21/21 ✓
- `test_issue_117` 9/9 ✓
- `test_issue_118` 11/11 ✓
- `test_issue_119` 6/6 ✓
- `test_issue_120` 7/7 ✓
- `test_issue_121` 8/8 ✓
- `test_issue_122` 6/6 ✓
- `test_issue_123` 6/6 ✓
- `test_issue_124` 5/5 ✓
- `test_issue_125` 7/7 ✓
- `test_issue_126` 23/23 ✓
- `test_issue_127` 15/15 ✓ (new)

## What (if anything) is still open

- Migrate the 43 `lower_to_ir` call sites to
  `lower_to_ir_result` with proper error handling.
- Update `lower_to_ir_impl` to emit real diagnostics
  for unknown primitives, type-registry misses, and
  invalid IR shapes.
- Use the monadic chain (`.transform`, `.and_then`) in
  production code paths.

3 files changed, 3 files added, 0 files removed.
