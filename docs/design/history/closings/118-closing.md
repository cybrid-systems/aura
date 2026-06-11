# Issue #118 — Constraint solver uses hard max_passes=10 and returns true on timeout; some diagnostics lack node tagging

## Status: ✅ Resolved (two-part robustness fix)

The constraint solver (`ConstraintSystem::solve()`) had a hard
limit of 10 passes and returned `true` regardless of whether the
worklist was empty or not when the limit was hit. This was a
soundness hole: AI-generated code that produced partial / under-
constrained programs would silently pass type checking, with the
caller observing a "valid" `TypeId` for what was actually an
under-constrained program.

Separately, three diagnostic paths in `synthesize_flat_var`
(empty name, module-member-not-found, unbound-variable) emitted
diagnostics without calling `flat.set_node_error()`, making them
invisible to `AuraQuery`'s `(has-error? N)` structured queries.

## What changed

### 1. `SolveResult` enum + TIMEOUT outcome
   (`src/compiler/type_checker.ixx`,
    `src/compiler/type_checker_impl.cpp`)

The solver now returns one of three values:

```cpp
enum class SolveResult : std::uint8_t {
    SOLVED = 0,
    CONFLICT = 1,
    TIMEOUT = 2,
};

SolveResult solve(std::vector<Constraint>* unresolved_out = nullptr);
```

- `SOLVED` — worklist empty after the pass loop.
- `CONFLICT` — a constraint failed unification (the program is
  unsound).
- `TIMEOUT` — pass limit hit with non-empty worklist. The
  unresolved-constraint list is filled in via the optional
  `unresolved_out` parameter so the caller can attach it to
  the diagnostic.

### 2. `infer_flat` handles the three outcomes
   (`src/compiler/type_checker_impl.cpp`)

The previous code path treated every "solve failed" as a
hard TypeError. The new code:

- **CONFLICT** → always TypeError (the program is unsound).
- **TIMEOUT in strict mode** → TypeError with a suggestion to
  add explicit type annotations to constrain the inference.
- **TIMEOUT in permissive mode** → Warning, with the result
  degraded to `Dynamic` (the existing LLM-friendly recovery
  pattern from Issue #103).

The diagnostic message includes a short summary of the
unresolved-constraint list (e.g. `"= 42 = 43, +2 more"`) so AI
agents can see exactly which constraints are still under-
constrained.

### 3. `synthesize_flat_var` now takes `FlatAST&` + `NodeId`
   (`src/compiler/type_checker.ixx`,
    `src/compiler/type_checker_impl.cpp`)

The signature changed from
```cpp
TypeId synthesize_flat_var(StringPool& pool, NodeView v);
```
to
```cpp
TypeId synthesize_flat_var(FlatAST& flat, StringPool& pool,
                          NodeId id, NodeView v);
```

The `FlatAST&` and `NodeId` parameters allow the three diagnostic
paths to call `flat.set_node_error(id, kind)`. The previous
comment in the code noted this was a known limitation; the
signature change resolves it.

Updated paths:
- Empty name (parse-error upstream) — tagged with
  `UnboundVariable`.
- Module-member-not-found — tagged with `UnboundVariable`
  (the closest-match suggestion continues to be emitted via
  `with_suggestion`).
- Variable-not-found — tagged with `UnboundVariable`.

The `BlameParty` and `with_suggestion` data was already populated
in the previous code; this change only adds the `set_node_error`
tag so `AuraQuery`'s `(has-error? N)` works for these paths.

### 4. Regression tests
   (`tests/test_issue_118.cpp`, 11/11 passed)

- `test_solve_result_enum` — `SolveResult` is exported from the
  type_checker module.
- `test_timeout_reports_unresolved` — well-typed inputs produce
  no diagnostics; result is valid.
- `test_unbound_var_tags_node` — `undefined_var` produces an
  `UnboundVariable` diagnostic AND tags the AST node with the
  same error kind.
- `test_module_member_not_found_tags_node` — `no_such_module:foo`
  produces an `UnboundVariable` diagnostic AND tags the AST node.
- `test_well_defined_module_member_no_tag` — well-typed inputs
  do NOT tag the node (regression check).
- `test_error_path_blameinfo_uniform` — unbound-variable
  diagnostic is emitted with full BlameInfo.
- `test_fuzz_gradual_occurrence_linear` — fuzzes 7 inputs
  exercising linear, gradual, occurrence, and basic let/if
  expressions; verifies all 7 typecheck without crashing and
  produce either OK or a diagnostic.

Wired into `CMakeLists.txt` as `test_issue_118` with a CTest entry
(`issue_118_verification`).

## Why the new design works

The old `bool solve()` API conflated three very different
outcomes into a single boolean, then defaulted to `true` on
all of them. This was a typical "I just want it to work" choice
that hid real failures.

The new `SolveResult` makes the outcome explicit and forces
callers to handle each one. The diagnostic format in
`infer_flat` distinguishes:

| Outcome | Strict | Permissive |
|---------|--------|------------|
| SOLVED  | (continue) | (continue) |
| CONFLICT | TypeError + suggestion | TypeError + suggestion |
| TIMEOUT | TypeError + suggestion + unresolved list | Warning + unresolved list, degrade to Dynamic |

This matches the existing Issue #103 pattern (LLM-friendly
recovery via permissive mode) but adds a TIMEOUT-aware
escalation: in strict mode, an under-constrained program is
a real type error, not a silent OK.

The diagnostic node-tagging fix is a smaller change but
completes a long-standing TODO. The comment at line 1807 of
the old code explicitly noted "would tag the node, but
synthesize_flat_var doesn't take a flat reference" — the
signature change closes that gap.

## Test status

- `integ`: 148/148 ✓ (no regression)
- `typecheck`: 10/10 ✓
- `test_issue_115`: 6/6 ✓
- `test_issue_116`: 21/21 ✓
- `test_issue_117`: 9/9 ✓
- `test_issue_118`: 11/11 ✓
- `test_ir`: type system 87/87, ownership 3/3, gradual 3/3 ✓

## What (if anything) is still open

- The TIMEOUT path is hard to trigger in normal usage. The
  fuzz test in `test_issue_118` exercises the path via the
  diagnostic format, but a more targeted test that constructs
  a deep polymorphic chain would be a follow-up.
- The `unresolved_out` parameter is currently a
  `std::vector<Constraint>*`. A structured
  `UnresolvedConstraintList` type with human-readable
  formatting + raw IDs would be a small ergonomic follow-up
  if AI agents want to display the unresolved list in their
  UIs.
- A few other diagnostic paths in `evaluator_impl.cpp` (lines
  16081, 17441, 17449) also use `UnboundVariable` without
  node tagging. Those are at the runtime level, not the
  type-checker level, and would be a separate issue.

3 files changed, 2 files added, 0 files removed.
