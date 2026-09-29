#!/usr/bin/env python3
"""Issue #4228 standard-face source-cite gate: filter prim-as-arg completion.

The Soft-face fix (fecdf639e) landed the Soft oneshot filter with std/math
preds. Its runtime cell handling is face-agnostic — the standard face is
completed by the same surface:

  - evaluator_primitives_list.cpp apply_unary / apply_pred / apply_binary
    deref define-cells before dispatch (is_cell -> ev.cells()[ci]) so a
    std/math pred reaching the native list hot paths through the standard
    oneshot pipeline is called, not dropped;
  - the filter prim snapshots cars under lock (same shape as map) so Soft
    std/math closures allocating via apply_closure cannot invalidate the
    live pairs_ walk in the direct form `(filter even? (list ...))`;
  - main.cpp's oneshot std prelude registers the TW export cells into the
    IR value-cell bindings (cs.sync_soft_export_cells_for_ir()) right
    after the `require "std/list|string|hash|math" all:` block, so
    TopCellLoad feeds native filter/map the live closure ids.

Evidence note: the direct form reproduced as "argument 0: expected
(__t -> Bool), got Any" + empty stdout only against a stale binary; a
controlled rebuild at the same tree passes all four direct cases, so the
completion surface is the runtime cell handling above (a type-checker
prim-env registration for the preds was attempted and proven
behavior-neutral — the module-export binding from the prelude require
shadows it — and reverted to avoid a checker/runtime divergence in
non-oneshot modes). The residual strict-face diagnostics on stderr are
pre-existing noise shared by the already-green number?/partition faces.

ACs:
  AC1  evaluator_primitives_list.cpp derefs define-cells in apply_unary /
       apply_pred / apply_binary before dispatch, citing #4228 (the
       face-agnostic completion the standard face rides on).
  AC2  the filter prim snapshots cars under lock citing #4228, and
       main.cpp's oneshot std prelude calls
       cs.sync_soft_export_cells_for_ir() with the #4228 TopCellLoad cite.
  AC3  check_oneshot_filter_4228.py carries all 9 cases — the four
       direct math-pred cases restored WITHOUT soft_only gating (case
       tuples are 3-wide: label/expr/expect; no soft_only element) —
       so all 9 run against the one standard build/aura binary.
  AC4  build.py registers this linter in the static-checks gate chain
       and scripts/coverage/root_check_allowlist.txt lists
       check_filter_prim_infer_standard.py.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIST_CPP = ROOT / "src" / "compiler" / "evaluator_primitives_list.cpp"
MAIN_CPP = ROOT / "src" / "main.cpp"
SMOKE = ROOT / "scripts" / "check_oneshot_filter_4228.py"
BUILD = ROOT / "build.py"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

FAILURES: list[str] = []


def check(ac: str, cond: bool, detail: str) -> None:
    if cond:
        print(f"PASS {ac}: {detail}")
    else:
        print(f"FAIL {ac}: {detail}")
        FAILURES.append(ac)


def main() -> int:
    if not LIST_CPP.is_file():
        print(f"FAIL missing {LIST_CPP}")
        return 1
    list_cpp = LIST_CPP.read_text(encoding="utf-8")

    # AC1 — define-cell deref before dispatch in all three apply helpers.
    check(
        "AC1 apply_unary cell deref",
        "// Issue #4228: deref define-cells (Soft std/math preds) before dispatch." in list_cpp
        and "if (is_cell(callee))" in list_cpp,
        "apply_unary derefs define-cells before dispatch (Issue #4228 cite)",
    )
    check(
        "AC1 apply_pred cell deref",
        "// Issue #4228: Soft std/math exports are define-cells. Env::lookup" in list_cpp
        and "if (is_cell(pred))" in list_cpp,
        "apply_pred tolerates a define-cell reaching the list hot path (Issue #4228 cite)",
    )
    check(
        "AC1 apply_binary cell deref",
        "// Issue #4228: deref define-cells before dispatch." in list_cpp,
        "apply_binary derefs define-cells before dispatch (Issue #4228 cite)",
    )

    # AC2 — filter car snapshot + prelude cell sync.
    check(
        "AC2 filter car snapshot",
        "// Issue #4228: snapshot cars under lock (same shape as map), then" in list_cpp,
        "the filter prim snapshots cars under lock (Issue #4228 cite)",
    )
    if not MAIN_CPP.is_file():
        print(f"FAIL missing {MAIN_CPP}")
        return 1
    main_cpp = MAIN_CPP.read_text(encoding="utf-8")
    check(
        "AC2 oneshot prelude cell sync",
        "// Issue #4228: Soft TW std/math preds must resolve via TopCellLoad" in main_cpp
        and "cs.sync_soft_export_cells_for_ir();" in main_cpp,
        "main.cpp oneshot prelude syncs TW export cells into IR value bindings (#4228)",
    )

    # AC3 — the 4228 smoke carries 9 ungated cases incl. the four direct
    # math-pred forms.
    if not SMOKE.is_file():
        print(f"FAIL missing {SMOKE}")
        return 1
    smoke = SMOKE.read_text(encoding="utf-8")
    check(
        "AC3 case tuple is 3-wide",
        "list[tuple[str, str, str]] = [" in smoke,
        "no soft_only 4th tuple element (ungated)",
    )
    smoke_code = "\n".join(ln for ln in smoke.splitlines() if not ln.lstrip().startswith("#"))
    check(
        "AC3 no soft_only gating logic",
        "soft_only" not in smoke_code,
        "check_oneshot_filter_4228.py has no soft_only gating logic (code lines)",
    )
    for label, expr in (
        ("direct-even?", "(filter even? (list 1 2 3 4))"),
        ("direct-odd?", "(filter odd? (list 1 2 3 4))"),
        ("direct-positive?", "(filter positive? (list -1 0 2 -3 4))"),
        ("direct-zero?", "(filter zero? (list 0 1 0 2))"),
        ("let-bind", "(let ((f filter) (e even?)) (f e (list 1 2 3 4)))"),
        ("apply", "(apply filter (list even? (list 1 2 3 4)))"),
        ("lambda", "(filter (lambda (x) (even? x)) (list 1 2 3 4))"),
        ("number?", "(filter number? (list 1 2 #f 3))"),
        ("partition", "(partition even? (list 1 2 3 4))"),
    ):
        row = f'("{label}", "{expr}"'
        check(f"AC3 case {label}", row in smoke, f"smoke case `{row}` present")
    check("AC3 nine cases", smoke.count('("') >= 9, "at least 9 case rows present")

    # AC4 — gate wiring: build.py registration + frozen allowlist row.
    if not BUILD.is_file():
        print(f"FAIL missing {BUILD}")
        return 1
    build = BUILD.read_text(encoding="utf-8")
    check(
        "AC4 build.py registers linter",
        "check_filter_prim_infer_standard.py" in build,
        "build.py static-checks chain runs scripts/check_filter_prim_infer_standard.py",
    )
    if not ALLOWLIST.is_file():
        print(f"FAIL missing {ALLOWLIST}")
        return 1
    allow = ALLOWLIST.read_text(encoding="utf-8").splitlines()
    check(
        "AC4 allowlist row",
        "check_filter_prim_infer_standard.py" in [ln.strip() for ln in allow],
        "scripts/coverage/root_check_allowlist.txt lists check_filter_prim_infer_standard.py",
    )

    if FAILURES:
        print(f"summary: {len(FAILURES)} failed")
        return 1
    print("summary: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
