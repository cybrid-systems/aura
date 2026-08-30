#!/usr/bin/env python3
"""Issue #3074: structural mutate:* Guard acquire is mutate_dispatch only.

Residual of #1964 cycle 4: mutate_dispatch.hh was a metrics-only stub;
each add_mutate body called MutationBoundaryGuard::try_acquire itself.
#3074 makes mutate_dispatch_try_acquire the sole acquire so new prims
cannot miss the Guard.

Contract (one row per AC):
  AC1 Structural mutate:* route through mutate_dispatch_try_acquire.
  AC2 GUARD_EXEMPT metadata prims stay exempt.
  AC3 MutateDispatchMetrics applied/rejected are live (no simulate).
  AC4 Existing mutation-guard --strict stays green.
  AC5 Residual direct MutationBoundaryGuard::try_acquire in
      evaluator_primitives_mutate.cpp (non-comment) == 0.
  AC6 Soft path unchanged; no docs/design/*; no invent test file.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    hh = _read("src/compiler/mutate_dispatch.hh")
    guard_lint = _read("scripts/coverage/checks/check_mutation_guard_coverage.py")
    t = _read("tests/compiler/test_mutation_guard_try_acquire_unit.cpp")
    t1964 = _read("tests/python/test_architectural_simplification_1964.py")
    build = _read("build.py")

    must("mutate_dispatch_try_acquire", "AC1", hh)
    must("kMutateDispatchSoleGuardIssue = 3074", "AC1", hh)
    must("mutate_dispatch_try_acquire", "AC1", mut)
    for name in (
        "mutate:replace-pattern",
        "mutate:query-and-replace",
        "mutate:replace-subtree",
        "mutate:rename-symbol",
        "mutate:move-node",
        "mutate:atomic-batch",
        "mutate:set-body",
        "mutate:rebind",
        "mutate:insert-child",
        "mutate:remove-node",
        "mutate:tweak-literal",
    ):
        must(name, "AC1", mut)

    must("GUARD_EXEMPT:", "AC2", mut)
    must("guard_exempt", "AC2", mut)

    # Issue #3452: raw add("mutate: only MetadataGuardExempt + GUARD_EXEMPT.
    # add_mutate("mutate: is the structural path (not matched here).
    lines = mut.splitlines()
    for i, ln in enumerate(lines, 1):
        s = ln.lstrip()
        if s.startswith("//") or s.startswith("*"):
            continue
        if 'add("mutate:' not in ln:
            continue
        win = "\n".join(lines[max(0, i - 12) : i + 16])
        if "GUARD_EXEMPT" not in win or "MetadataGuardExempt" not in win:
            fails.append(f'AC3452: raw add("mutate: at mutate.cpp:{i} is not GUARD_EXEMPT + MetadataGuardExempt')
    for name in (
        "mutate:replace-type",
        "mutate:replace-value",
        "mutate:replace-pattern",
        "mutate:atomic-batch",
        "mutate:set-body",
        "mutate:rebind",
    ):
        pos = mut.find(f'"{name}"')
        if pos < 0:
            fails.append(f"AC3452: missing {name}")
            continue
        look = mut[max(0, pos - 80) : pos]
        if "add_mutate" not in look:
            fails.append(f"AC3452: {name} is not registered through add_mutate")
    must("MutateRegKind", "AC3452 kind", hh)
    must("kMutateRegKindIssue = 3452", "AC3452 stamp", hh)

    must("mutate_dispatch_note", "AC3", hh)
    if "simulate applied" in hh:
        fails.append("AC3: simulate applied path still present")
    must("rejected_total", "AC3", hh)
    must("schema-3074", "AC3", mut)

    must("mutate_dispatch_try_acquire", "AC4 coverage linter", guard_lint)

    residual = 0
    for i, ln in enumerate(mut.splitlines(), 1):
        s = ln.lstrip()
        if s.startswith("//") or s.startswith("*"):
            continue
        if "MutationBoundaryGuard::try_acquire" in ln:
            residual += 1
            fails.append(f"AC5: residual try_acquire at mutate.cpp:{i}")
    if residual != 0:
        fails.append(f"AC5: expected 0 residual try_acquire, found {residual}")

    must("ac3074_1_structural_routes_dispatch", "AC5", t)
    must("mutate_dispatch_note(MutateKind::SetBody", "AC5 1964", t1964)
    must("check_mutate_dispatch_sole_guard_3074", "AC5", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3074.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3074.cpp present (forbidden invent)")

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3074-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3074 mutate_dispatch sole Guard — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
