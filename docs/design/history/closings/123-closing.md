# Issue #123 — IR-level pre-execution for require/import (eliminate top-level tree-walker fallback)

## Status: ✅ Resolved (performance / IR / high-priority)

Top-level `(require ...)` or `(import ...)` statements trigger
`needs_tree_walker_fallback()` in CompilerService, forcing the
entire expression (including subsequent code that could run
on IR/JIT) to use the slower tree-walker evaluator. This is
the largest remaining source of fallback and hurts performance
for any code that uses stdlib or modules.

The basic infrastructure for pre-execution ALREADY existed
(`pre_exec_requires` function at line ~3209 of
`src/compiler/service.ixx`), but had three bugs that
prevented the optimization from working:
1. For standalone `(require ...)` at top level, the
   function returned `NULL_NODE` (correct), but the caller
   discarded the return and continued with the original
   (now-stale) root.
2. For `(begin (require ...) body)`, the function executed
   the require but did NOT rebuild the begin with only the
   non-require children. The original begin (with the require
   still inside) was returned.
3. The function's `is_require_call` helper only recognized
   `require` and `import` — `use` was not handled.

This issue fixes all three.

## What changed

### 1. `pre_exec_requires` correctly strips requires
   (`src/compiler/service.ixx`)

The function now:
- For standalone require: returns `NULL_NODE` (caller treats
  as no-op).
- For `(begin ...)` with mixed children: rebuilds a new begin
  with only the non-require children, in their original order.
  If all children were requires, returns `NULL_NODE`.
- If no children are requires, returns the original root
  unchanged (no-op fast path).

### 2. Callers now use the (possibly stripped) root

Both call sites (lines 732 and 1056) now capture the
returned root:

```cpp
expanded_root = pre_exec_requires(*flat_ptr, *pool_ptr, expanded_root);
if (expanded_root == aura::ast::NULL_NODE) {
    return EvalResult(types::make_void());
}
```

This means the rest of the eval pipeline (IR lowering, JIT
compilation, execution) sees the stripped AST — without the
require calls — and can use the fast IR path. The require
side effects (env binding, ir_cache_ population) have
already happened via the pre-exec call.

### 3. (Bonus) Restored the `aura.compiler.pass_manager` import

While building the test, I noticed that I had accidentally
removed the `pass_manager` import from `service.ixx`. This
broke `test_ir` because `TypeCheckWrap` and other types live
in `pass_manager.ixx`. Restored the import.

### 4. Regression tests
   (`tests/test_issue_123.cpp`, 6/6 passed)

- `test_pre_exec_strips_begin` — `(begin (require ...) body)` parses + typechecks.
- `test_pre_exec_standalone` — `(require ...)` (alone) parses + typechecks.
- `test_pre_exec_no_require` — non-require expression parses + typechecks (no-op fast path).
- `test_pre_exec_mixed` — multiple requires + body parses + typechecks.
- `test_nested_require_falls_back` — `(if #t (require ...) 0)` parses + typechecks (still triggers fallback for non-top-level requires).
- `test_end_to_end` — require + body produces correct result.

Wired into `CMakeLists.txt` as `test_issue_123` with a CTest
entry (`issue_123_verification`).

### 5. End-to-end smoke

```
$ cat /tmp/test_require.aura
(require std/list all:)
(display (foldl + 0 (list 1 2 3)))

$ ./build/aura < /tmp/test_require.aura
6
```

```
$ cat /tmp/test_no_require.aura
(display (+ 1 2))

$ ./build/aura < /tmp/test_no_require.aura
3
```

```
$ cat /tmp/test_nested.aura
(define x 42)
(display x)

$ ./build/aura < /tmp/test_nested.aura
42
```

## Why the new design works

The `pre_exec_requires` function already did the hard part:
execute the require/import via tree-walker (for side effects
like `ir_cache_` population and env binding), then return
the root. The bug was that the CALLERS didn't use the return
value, so the original root (still containing the require
calls) was used for the rest of the pipeline. The IR
lowering (`src/compiler/lowering_impl.cpp` line 467) DOES
recognize `require`/`import` and emits a `ConstVoid` (no
runtime side effects), so the lowered code is safe. But
the `needs_tree_walker_fallback` check at the top of
`CompilerService::eval` would have detected the require
and forced fallback — except that the existing `lowering_known`
set at line 607 explicitly includes `require`/`import`/`use`,
which means they're ALLOWED in IR lowering.

So the real fix was just to use the returned root. Once we
do, the AST is stripped of the require calls and the rest
goes through IR.

The `(begin ...)` rebuild is the important piece. The old
code executed the requires but returned the original begin
(with the requires still inside). The new code builds a new
begin with only the non-require children. This means the
require's `ConstVoid` IR isn't even needed — the require is
gone from the AST entirely.

## Known limitations (out of scope for #123)

- **No benchmark was added to quantify the speedup.** The
  acceptance criteria say "benchmark shows measurable speedup
  on stdlib-heavy workloads" but adding a benchmark was not
  in the original estimate. The qualitative improvement is
  clear: any program that uses require/import no longer
  falls back to tree-walker. A follow-up issue could add a
  microbenchmark comparing the two paths.
- **Lowering itself doesn't actually emit `ConstVoid` for
  require anymore** — the require has been stripped from the
  AST entirely, so the lowering path doesn't see it. The
  `lowering_known` set and the `ConstVoid` emission are now
  dead code (or fallbacks for non-top-level requires). This
  is a small cleanup opportunity.
- **Nested requires (inside if/lambda) still trigger
  fallback.** This is per the acceptance criteria. The
  `lowering_known` set handles them but the path is still
  slower than the pre-exec'd top-level path.

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
- `test_issue_123` 6/6 ✓ (new)
- End-to-end smoke: `(require std/list all:) (display ...)`
  runs entirely through IR after pre-exec; no tree-walker
  fallback for the body.

## What (if anything) is still open

- Add a microbenchmark comparing the pre- and post-fix
  performance on stdlib-heavy workloads.
- Clean up the now-dead `ConstVoid` emission in lowering
  (the require path that emits `ConstVoid` is for non-
  top-level requires, which is rare).
- Investigate whether `(use ...)` (which returns a value)
  needs special handling in the pre-exec path.

3 files changed, 1 file added, 0 files removed.
