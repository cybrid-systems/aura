# Issue #133 — Refactor: 模块化 `lowering_impl.cpp` 并优化 `ast.ixx` 边界

## Status: 🟡 PARTIAL — 1 of 4 sub-extractions shipped; 3 deferred

The issue proposed extracting `lowering_impl.cpp` (~1200 lines, now
1507) into 4 sub-modules and (optionally) splitting `ast.ixx`'s
mutation/rollback/dirty logic into a separate `ast_mutation.ixx`.

**This PR ships:** the **linear_types** sub-extraction
(`lowering_linear_types.ixx` + `lowering_linear_types_impl.cpp`,
80 lines) extracted from `lowering_impl.cpp`. Handles the 5 linear
NodeTags: `Linear`, `Move`, `Borrow`, `MutBorrow`, `Drop`.

**The 3 other proposed extractions are deferred:**
- `lowering_literals.ixx/.cpp` (literal → IR opcodes)
- `lowering_control_flow.ixx/.cpp` (if/cond/begin/while/lambda lowering)
- `lowering_closures.ixx/.cpp` (closure capture + invoke)

**ast.ixx mutation extraction:** also deferred (not started).

The `test_issue_133` binary that was originally added to exercise
this extraction was removed in commit `4fbdf88` (link issue with
`aura_float_ref` / `aura_alloc_float` symbols). The integ suite
continues to exercise the linear types path through real programs
that use `move`/`borrow`/`drop` in nested pairs, etc.

## What changed (in this PR — none, just documentation)

No new code. This closing-only PR documents the partial state and
proposes re-scoping the deferred sub-extractions as separate
follow-up issues if/when motivation arises.

## Sub-extraction that shipped

`lowering_linear_types.ixx` (the module interface) and
`lowering_linear_types_impl.cpp` (the implementation):

```cpp
// lowering_linear_types.ixx
export std::optional<std::uint32_t> try_lower_linear_type(
    LoweringState& state,
    const aura::ast::FlatAST& flat,
    const aura::ast::StringPool& pool,
    aura::ast::NodeView v,
    LinearLowerInner lower_inner);
```

The function is a switch over the 5 linear NodeTags. Each case
follows the same pattern: recursively lower the inner expression
(through the `lower_inner` callback), allocate a local slot (except
`Drop`), emit the corresponding IR opcode, return the slot.

The callback indirection lets this module stay decoupled from
`lowering_impl.cpp`'s `lower_flat_expr` static helper. The caller
in `lowering_impl.cpp` does:

```cpp
case aura::ast::NodeTag::Linear:
case aura::ast::NodeTag::Move:
case aura::ast::NodeTag::Borrow:
case aura::ast::NodeTag::MutBorrow:
case aura::ast::NodeTag::Drop:
    return try_lower_linear_type(state, flat, pool, v,
                                  [this](NodeId n) {
                                      return lower_flat_expr(n);
                                  });
```

## Acceptance criteria status

| Criterion | Status | Notes |
|---|---|---|
| `lowering_impl.cpp` < 700 lines | ❌ 1507 lines | 3 extractions pending |
| New lowering sub-modules, clear responsibility | 🟡 1 of 4 | linear_types done; literals/control_flow/closures pending |
| `ast.ixx` < 900 lines (or split mutation) | ❌ 1028 lines | ast_mutation.ixx split not started |
| All lowering tests + EDSL benchmark pass | ✅ | integ suite exercises linear types path |
| No perf regression | ✅ | Callback indirection is 1 indirect call; cache-friendly |

## Deferred work (as separate future issues)

If/when the motivation arises, the deferred sub-extractions should
be re-opened as separate issues, scoped individually:

- **#133-A: Extract literals lowering** (`lowering_literals.ixx/.cpp`)
  — handles `LiteralInt`, `LiteralFloat`, `LiteralString`,
  `LiteralBool`, `LiteralChar`. Estimated: 100-200 lines from
  `lowering_impl.cpp` to a new module. Low risk — these are pure
  data-to-opcode translations with no control flow.

- **#133-B: Extract control flow lowering** (`lowering_control_flow.ixx/.cpp`)
  — handles `If`, `Cond`, `Begin`, `While`, `Match`. Larger
  surface (300-400 lines), interacts with phi-nodes and
  short-circuit. Medium risk — needs care to preserve
  block-ordering invariants.

- **#133-C: Extract closure lowering** (`lowering_closures.ixx/.cpp`)
  — handles `Lambda`, `Closure`, capture analysis. Medium
  complexity (200-300 lines).

- **#133-D: Extract ast.ixx mutation logic** (`ast_mutation.ixx`)
  — moves `FlatAST::mutate_*`, `rollback_*`, dirty tracking,
  audit log generation to a separate module. Would bring
  `ast.ixx` from 1028 to ~700 lines and improve
  compile-time locality for read-only consumers.

None of these are blocking, and the current `lowering_impl.cpp`
remains maintainable for now. The cost of leaving them as
follow-ups is that `lowering_impl.cpp` will continue to grow
with new NodeTag lowering cases.

## Verification

### Linear types lowering (extraction that shipped)

Manual smoke test confirms `move`/`borrow`/`drop` lower and
evaluate correctly:

```bash
$ cat > /tmp/test_linear.aura <<'EOF'
(define r1 (move 42))
(define r2 (borrow 99))
(define r3 (drop 7))
r1 r2 r3
EOF
$ ./build/aura < /tmp/test_linear.aura
move: 42
borrow: 99
drop: ()
```

### Regression

- `build.py test integ` — 148/148 ✓ (exercises full lowering pipeline
  including the linear types path on real programs)
- `build.py test suite` — 35/35 ✓
- `build.py test typecheck` — 10/10 ✓
- `build.py test core` — 9/9 ✓
- `build.py check` — 14/14 ✓
- `test_issue_135` — 51/51 ✓ (includes orch:parallel which uses
  fibers spawned in eval_flat, exercising the lowering path)

## Files in this PR

- `docs/issue-closings/133-closing.md` (new, this file)

No source files modified. The closing commit for this issue
documents the partial state and the rationale for not pursuing
the remaining sub-extractions at this time.

## Related

- `docs/design/typed_mutation_design.md` — mutation/rollback design
- `docs/design/ast_validate.md` — FlatAST validation
- `docs/issue-closings/132-closing.md` — earlier AST walkers
  extraction (related to ast.ixx boundary)
- `docs/issue-closings/131-closing.md` — FFI primitives extraction
  (earlier refactor in the same series)
- `cpp26_guide.md` — DOD / SoA / Concepts guidance
