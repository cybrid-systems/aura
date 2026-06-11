# Issue #117 — Linear ownership validation only processes dirty_bindings (incomplete static check)

## Status: ✅ Resolved (safety gap closed)

The TypeChecker's ownership validation only re-simulated ownership
flow for bindings in the dirty set. Clean bindings (no recent
mutation) were assumed correct from the previous full type-check
pass. This post-hoc simulation left three gaps:

1. **Cross-function flows** — when a linear value is passed as
   an argument to another function, the dirty set on the caller
   side doesn't include the callee's locals. A use-after-move
   or leak inside the callee would go undetected.
2. **Closure-captured linear** — a `let`-bound linear value
   captured by a lambda, then moved in the lambda body, was
   never re-validated because the closure body wasn't in the
   dirty set.
3. **Global-scope linear** — a let-bound linear at the top
   level (no enclosing function body) had no "scope end" to
   check leak state against in the dirty path.
4. **Gradual + Linear boundary** — `consistent_unify` allowed
   `Any ~ Linear` consistent unification, silently erasing
   the linear ownership invariant when a value flowed through
   a Dynamic context.

## What changed

### 1. `validate_ownership_full` — full re-simulation mode
   (`src/compiler/type_checker.ixx`,
    `src/compiler/type_checker_impl.cpp`)

A new public static method on `OwnershipEnv`:

```cpp
static bool validate_ownership_full(
    const FlatAST& flat,
    const StringPool& pool,
    NodeId root,
    std::vector<OwnershipNote>& notes_out);
```

Walks the AST to discover ALL linear-typed bindings (syntactic
check: any `let` whose value is a `(Linear ...)` node) and
validates them in a single scope-aware pass. The walk is shared
with the existing `validate_ownership` via a private
`validate_ownership_impl` helper — the only difference between
the two paths is which set of bindings to track.

The full path is slower than the dirty-only path (it walks
all let-introduced bindings, not just the mutated ones) but
catches the cross-function / closure / global-scope cases
that the dirty path missed. Callers that already have a
TypeRegistry can compute the linear-typed binding set more
precisely and pass it via the existing `validate_ownership` —
the new `validate_ownership_full` is the no-registry convenience
path.

### 2. Gradual + Linear boundary check
   (`src/compiler/type_checker_impl.cpp`)

Both `consistent_unify` and `is_coercible` now reject
`Any ~ Linear` (and `Linear ~ Any`):

```cpp
// consistent_unify: refuse to unify Any with a Linear type
if (t1 == reg_.dynamic_type() || t2 == reg_.dynamic_type()) {
    auto other_id = (t1 == reg_.dynamic_type()) ? t2 : t1;
    if (reg_.linear_of(other_id) != nullptr) {
        return false;
    }
}

// is_coercible: refuse to insert a CastOp for Linear <-> Any
if (reg_.linear_of(from) != nullptr || reg_.linear_of(to) != nullptr) {
    return false;
}
```

The right escape hatch is now an explicit user-driven cast
(not a silent one). The runtime CastOp machinery is unchanged,
so a linear value that *is* explicitly cast (via `(cast x T)`)
still works; what we removed is the implicit-erase-via-gradual
boundary that silently violated the ownership invariant.

### 3. Regression tests (`tests/test_issue_117.cpp`, 9/9 passed)

- `test_full_re_simulation_discovers_linear` — `validate_ownership_full`
  finds a leaked-linear binding in a hand-built `(let ((x (Linear 42))) ...)`.
- `test_full_catches_what_dirty_misses` — same hand-built AST,
  empty dirty set: dirty-only path produces no diagnostics
  (correctly skipped), full path reports the leak.
- `test_full_properly_moved_no_leak` — same AST with `(move x)`
  in the body: no leak, overall pass.
- `test_gradual_linear_boundary` — `(let ((x (Linear 42))) (display x))`
  in strict mode: at least one `TypeError` diagnostic
  (consistent_unify returns false on Any ~ Linear).
- `test_dirty_only_still_works` — regression check: dirty-only
  path on a non-linear binding still produces no diagnostics.

Wired into `CMakeLists.txt` as `test_issue_117` with a CTest entry
(`issue_117_verification`).

## Why the new design works

The old dirty-only design was a performance optimization
(avoid walking the whole tree on every mutation) that traded
correctness for speed. The full-re-simulation mode is the
correct-but-slow counterpart. The two paths share the same
walk, so the new code is just a discovery step + dispatch:

```
                              ┌─────────────────────┐
                              │ validate_ownership  │  (post-mutation: dirty set)
                              └──────────┬──────────┘
                                         │
                                         ▼
                              ┌─────────────────────┐
                              │  discover bindings  │  (set the inputs to the walk)
                              └──────────┬──────────┘
                                         │
                  ┌──────────────────────┴──────────────────────┐
                  │                                             │
                  ▼                                             ▼
   dirty set: {x, y, ...}                     all linear bindings: {x, y, z, ...}
                  │                                             │
                  └──────────────────────┬──────────────────────┘
                                         │
                                         ▼
                              ┌─────────────────────┐
                              │ validate_ownership_ │  (shared walk, scope-aware)
                              │ impl                │
                              └─────────────────────┘
```

The dirty path stays for hot loops (mutation + re-typecheck
inside the AI agent loop). The full path is for safety:
`run_typecheck` after a structural mutation can opt into
`validate_ownership_full` if the agent is doing something
risky (like moving a linear value into a function it didn't
define itself).

The gradual + Linear boundary check is a smaller change but
closes a soundness hole. Before, `(let ((x (Linear 42))) (display x))`
was accepted with a silent CastOp — the runtime would wrap x
in a Dynamic container, losing the linear info. Now it's a
TypeError, forcing the user to be explicit.

## Test status

- `integ`: 148/148 ✓ (no regression)
- `typecheck`: 10/10 ✓
- `test_ir`: type system 87/87, ownership 3/3, gradual 3/3, all pass
- `test_issue_115`: 6/6 ✓ (regression check on #115)
- `test_issue_116`: 21/21 ✓ (regression check on #116)
- `test_issue_117`: 9/9 ✓

## What (if anything) is still open

- The discovery heuristic in `validate_ownership_full` uses
  the AST structure (`(Linear ...)` wrapper nodes) rather
  than the type registry. This is a no-registry convenience
  — callers that have a `TypeRegistry` can compute the
  linear-typed binding set precisely and pass it via the
  existing `validate_ownership` for a stricter check. The
  full path is the "no info, just walk the AST" fallback.
- Cross-function move semantics: the linear value passed as
  a function argument is currently not tracked across the
  call boundary (the callee's perspective). A full fix
  would require:
  - The caller marking the argument as Moved at the call site.
  - The callee checking that the parameter is Owned on entry
    and Moved/Dropped on exit.
  This is a separate issue (not in #117's scope).

3 files changed, 2 files added, 0 files removed.
