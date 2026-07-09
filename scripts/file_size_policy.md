# File size policy

This document is the source of truth for source-file size limits
in the Aura codebase. It pairs with `scripts/check_file_size.py`,
which enforces the limits (on-demand via `lint_file_size`, and
optionally in CI).

## Why

Large files are a clean-code liability:

- Hard to read — scrolling through 7000 lines breaks flow.
- Hard to review — PRs touching 100+ line files discourage deep
  review.
- Slow incremental compilation — every consumer of the module
  recompiles on any change to the interface unit.
- Hard to onboard — new contributors can't hold the file in
  their head.

The fix is to keep `.ixx` files focused on a single concern and
limited in size. We also split large files into `.ixx` + `.cpp`
pairs (declarations in the interface, implementations in a module
implementation unit), as established by #378 and #379.

## Thresholds

### `.ixx` (module interfaces) — Issue #382

| Threshold | Default | Meaning                                                  |
|-----------|---------|----------------------------------------------------------|
| Warning   | 800     | File is over the per-issue target. Schedule a split.     |
| Blocker   | 2000    | File is too large. Must be split before merging new code.|

### `.cpp` / `.h` (implementation + metrics headers)

| Threshold | Default | Meaning |
|-----------|---------|---------|
| Warning | 2500 | Schedule a split for non-capped files |
| Blocker | 5000 | New files must not land above this |
| **Caps** | per-path | Known mega-TUs in `FILE_LINE_CAPS` — **must not grow** past the listed line count |

Capped files (e.g. `evaluator.ixx`, `evaluator_primitives_observability.cpp`)
are reported as `capped` while under their freeze limit, and as
`blocker` if a PR grows them past the cap. Shrink and lower the
cap over time; remove the cap entry when under the normal blocker.

Use `--no-cpp` for legacy ixx-only behavior.

## Current state (as of #382 scope-limited first cut)

Run `python3 scripts/check_file_size.py` to see the live state.
Snapshot at the time this doc was written:

| File                                       | Lines | Status   |
|--------------------------------------------|-------|----------|
| `src/compiler/service.ixx`                 |  7364 | blocker  |
| `src/core/ast.ixx`                         |  5583 | blocker  |
| `src/compiler/evaluator.ixx`               |  4325 | blocker  |
| `src/compiler/pass_manager.ixx`            |  3196 | blocker  |
| `src/compiler/type_checker.ixx`            |  1268 | warning  |
| `src/compiler/ir.ixx`                      |   810 | warning  |

`src/core/arena.ixx` (791 lines) is just under the warning
threshold.

**33 of 39 `.ixx` files** are under the warning threshold.

## Follow-up splits (out of scope for #382 first cut)

The umbrella issue (#382) is the parent. Each of these is a
candidate for a separate issue + scope-limited first cut (the
pattern established by #378 / #379 / #380 / #381):

1. **`src/compiler/service.ixx`** (7364 lines) — biggest file.
   The CompilerService class is the top-level orchestrator and
   accumulates features from every prior issue. Splitting
   requires extracting a `service_internals.ixx` (or several)
   holding the helpers + the type-erased callback maps. The
   public API stays in `service.ixx` as a thin facade.
2. **`src/core/ast.ixx`** (5583 lines) — partially split by #378
   and #379. Remaining scope: the FlatAST class body itself
   (~4700 lines of inline methods) is still monolithic. Follow-
   up: extract member function bodies to `ast_impl.cpp` /
   `ast_stability.cpp` (already in use from #379) one method at
   a time, with friend access for private SoA columns.
3. **`src/compiler/evaluator.ixx`** (4325 lines) — the
   `Evaluator` class + the WORK scopes. Could split into
   `evaluator.ixx` (class + interface) + `evaluator_impl.cpp`
   (large method bodies) + `evaluator_scopes.ixx` (WorkScope,
   EvalScope, etc.).
4. **`src/compiler/pass_manager.ixx`** (3196 lines) — the pass
   classes (DCEPass, InlinePass, TCOPass, MonomorphizePass, etc.)
   are all in one file. Could split into `pass_manager.ixx`
   (concepts + run_pipeline + new helpers — already small after
   #381) + `pass_wraps.ixx` (ComputeKindWrap, ArityWrap,
   ConstantFoldingWrap, TypeCheckWrap) + `passes_analysis.ixx`
   (EscapeAnalysisPass, TypePropagationPass, LinearOwnershipPass)
   + `passes_transform.ixx` (InlinePass, TCOPass, DCEPass,
   MonomorphizePass, DeadCoercionEliminationPass,
   TypeSpecializationWrap).
5. **`src/compiler/type_checker.ixx`** (1268 lines) — over
   warning, under blocker. Could split impl to
   `type_checker_impl.cpp` (already exists as a 5059-line
   sibling — could itself be split, but that's `.cpp` not
   `.ixx`).
6. **`src/compiler/ir.ixx`** (810 lines) — barely over warning.
   Could split impl details to `ir_impl.cpp`.

## Usage

```bash
python3 scripts/check_file_size.py              # ixx + cpp/h
python3 scripts/check_file_size.py --no-cpp     # ixx only (#382)
python3 scripts/check_file_size.py --warning 500 --blocker 1000
python3 scripts/check_file_size.py --json > file_size.json
cmake --build build --target lint_file_size
```

## CI integration (future)

`scripts/check_file_size.py` is designed to plug into CI as a
pre-merge check. The `--json` flag emits a parseable summary
for dashboard ingestion. Future work:
- Wire the script into the GitHub Actions workflow.
- Add a job-comment bot that posts the file-size diff on PRs
  touching `.ixx` files.
- Add a per-PR trend (file size delta in the PR vs. main).

These are out of scope for the #382 first cut.
