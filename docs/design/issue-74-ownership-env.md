# Design: Issue #74 — OwnershipEnv scope-aware tracking + leak diagnostics

## Current state

`src/compiler/type_checker_impl.cpp:2414` (in `OwnershipEnv::validate_ownership`):

```cpp
// This is a simplified linear walk — it assumes ops appear in
// program order within a single scope. For full precision we'd
// need to track scope nesting, but this catches the main violations.
for (auto& op : ops) { ... }

// Check 2: any dirty linear binding still in Owned state at end of walk?
for (auto& name : dirty_bindings) {
    auto st = tmp_env.get(name);
    if (st == OwnershipState::Owned) {
        bool appeared = false;
        for (auto& op : ops) {
            if (op.target_var == name) { appeared = true; break; }
        }
        if (appeared) {
            // All ops passed but binding ended still Owned — that's fine.
            // The ownership check already validated proper transitions.
        }
    }
}
```

The two issues called out in the issue:

1. **Final Owned check is a no-op comment** — the `if (appeared) { /* that's fine */ }`
   branch never emits a diagnostic. A linear resource that was bound but
   never moved/dropped should be reported as **leaked**.

2. **No lexical scope tracking** — the walk assumes all ops happen in
   one flat scope. If a binding is moved in a let body but the let
   also declares other variables, the scope transitions aren't modeled.
   Specifically, when a let scope ends, any linear binding introduced
   in that scope should be in a non-Owned state (otherwise it's a leak
   in that scope, even if the outer scope never sees it).

## The bug in detail

### Bug A: leaked-linear not reported

```aura
(let ((x (mk-linear)))   ; x is linear, Owned
  (display x))           ; display doesn't move it
; x's scope ends here — should be a leak error
```

Current behavior: no diagnostic.
Expected: `leaked-linear: x was never moved or dropped at end of scope`.

### Bug B: scope nesting ignored

```aura
(let ((x (mk-linear)))
  (move x)        ; x is Moved in this scope
  (let ((y (mk-linear)))
    (move y))      ; y is Moved in inner scope
  ; inner scope ended — y was moved, OK
  ; x is still Moved from outer let body — outer scope's state
  ; is correct (x is in Moved state, not leaked)
```

Current behavior: this MIGHT work because `tmp_env` is flat (no scopes).
But if we have:

```aura
(let ((x (mk-linear)))
  (let ((y (mk-linear)))   ; new binding y in inner scope
    (move y))              ; y Moved in inner scope
  ; inner scope popped — y's state is gone (correct: y ended)
  ; x is still Owned in outer scope (never moved)
  ; end of outer scope — x is leaked
```

Current behavior: x is in `tmp_env.scopes_` (single global scope) as
Owned, so the final check `if (st == Owned)` fires, but the `appeared`
flag is true (we have ops, just not for x), so the diagnostic is
suppressed by the `if (appeared)` comment. No leak reported.

Expected: `leaked-linear: x was never moved or dropped at end of scope`.

## The fix

Two parts, in `OwnershipEnv::validate_ownership`:

### Part 1: scope stack during collection

When collecting ops, also track which scope each binding belongs to.
We need to know when a scope starts (let/lambda/if body) and ends.
The flat walk already visits the AST in source order. We can
maintain a scope depth counter:

- Increment on `Let` (after binding) or `Lambda` body or `If` branch
- Decrement on scope exit
- Track per-scope bindings (name → state)

When a scope ends, check all linear bindings introduced in that
scope. If any is still `Owned` (never moved/dropped), emit a
`leaked-linear` note.

### Part 2: real leak diagnostic

Replace the no-op `if (appeared) { /* that's fine */ }` with:

```cpp
if (appeared) {
    // All ops passed but binding ended still Owned — that's a leak.
    // The variable is linear-typed and was never moved or dropped.
    notes_out.push_back({last_op_for_var_or_root,
                         "leaked linear resource: " + name,
                         "leaked-linear"});
    all_pass = false;
}
```

Actually no — `appeared` doesn't help here. The real check is: was
this binding ever touched by any op? If yes and it ended Owned, it's
leaked. If no and it ended Owned, it was declared but never used —
could be intentional (the user just wants the side effect of the
constructor) or could be a bug. We can't always tell, so we should
emit a soft warning.

But the issue's main complaint is that the current code SILENTLY
drops the leak. So the fix is: ALWAYS emit a note when a linear
binding ends in Owned state at the end of its scope.

### Implementation strategy

Build a real scope-aware walker that:
1. Maintains a scope stack
2. On scope enter, snapshots the current set of bindings
3. On scope exit, checks bindings in the snapshot
4. Recurses into children
5. For each Move/Borrow/MutBorrow/Drop op, tracks state transitions

```cpp
struct ScopeInfo {
    std::unordered_set<std::string> introduced;  // bindings declared in this scope
    std::unordered_map<std::string, OwnershipState> end_state;  // state at end
};
std::vector<ScopeInfo> scope_stack;

auto on_scope_enter = [&]() {
    scope_stack.push_back({});
};

auto on_scope_exit = [&](NodeId exit_node) {
    auto info = scope_stack.back();
    for (auto& name : info.introduced) {
        if (dirty_bindings.count(name) == 0) continue;
        auto st = tmp_env.get(name);
        if (st == OwnershipState::Owned) {
            // Leak: linear resource declared but never moved/dropped
            notes_out.push_back({exit_node,
                                 "leaked linear resource: " + name,
                                 "leaked-linear"});
            all_pass = false;
        }
    }
    scope_stack.pop_back();
};
```

For scope-enter/exit identification, we use the AST node tags:
- `NodeTag::Let` (when entering the let body, AFTER the binding)
- `NodeTag::Lambda` (when entering the lambda body)
- `NodeTag::If` (each branch is a separate scope)
- `NodeTag::Begin` (each expression in a begin is a separate scope? or one scope for the whole begin?)

For simplicity, I'll use:
- `Let`: enter scope for body
- `Lambda`: enter scope for body
- `If`: enter separate scopes for then/else
- `Begin`: one scope for the whole begin (no per-expression scope)

This matches common Lisp semantics.

### Detecting which bindings are "introduced" in a scope

The simplest way: a binding is introduced in the scope where its
`Let` (or `Lambda` parameter) appears. We track this by adding the
bound name to the current scope's `introduced` set when we process
the Let/Lambda node.

For Lambda parameters, the parameters are bound before the body, so
we can add them at the same time we push the scope.

### Limitations

- We don't track **transitive** leaks (e.g. a linear resource
  stored in a record field that's never accessed). This is a known
  limitation of the current ownership simulation. The fix in this
  issue only handles direct leaks.
- We don't model **scope nesting for nested let/lambda**. Each
  let/lambda introduces its own scope; popping checks only that
  scope's bindings. Outer bindings are checked at the outer scope's
  exit.
- The current code uses `tmp_env` which is a fresh env. We need to
  coordinate `tmp_env`'s scope stack with the walker.

## Test plan

Add tests to `tests/test_ir.cpp` (new "OWN" section) that verify:

1. **Linear resource never moved is reported as leaked** (Bug A)
2. **Linear resource moved in inner scope, leaked in outer scope** (Bug B)
3. **Linear resource properly moved at end of scope, no leak**
4. **Linear resource moved before end of scope, no leak**

We can construct test FlatASTs by hand:
```cpp
// Build: (let ((x linear-var)) x)  — x is never moved
auto lin_var = pool.intern("x");
auto lin_ref = flat.add_variable(lin_var);
auto move_node = flat.add_move(lin_ref);
auto let_node = flat.add_let(lin_var, /*value=*/some_int, /*body=*/move_node);
```

Then call `validate_ownership` and check that notes contains a
`leaked-linear` note for "x".

## Implementation plan

1. Rewrite the walk in `validate_ownership` to be scope-aware
2. Add leak diagnostic on scope exit
3. Add 2-4 new test cases in `tests/test_ir.cpp`
4. Run all tests for regressions

## Acceptance criteria

- A linear resource declared but never moved/dropped at end of
  scope produces a `leaked-linear` note
- A linear resource properly moved produces no leak note
- The existing "use-after-move", "double-borrow", "invalid-state"
  notes still work (no regression)
- All existing 30/30 suite, 5/5 leak, 201/201 run-tests,
  67/67 TS tests still pass

## Out of scope

- Transitive leaks (linear resource inside a record)
- Substructural type system (full linear type checker with subtyping)
- Region-based memory management
- Liveness analysis for non-linear resources
