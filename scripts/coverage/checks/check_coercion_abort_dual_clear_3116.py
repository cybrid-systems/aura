#!/usr/bin/env python3
"""Issue #3116: production abort dual-clear CoercionMap + TLS context.

After apply_coercion_map, force-rollback / cancel must drop last_coercions_
and clear_coercion_active_mutation_context so topology restore is not
half-green. Soft/Off: observe only.

Contract (one row per AC):
  AC1  dual_clear_coercion_state_on_abort takes last_coercions_ + clears TLS
  AC2  Wired on exit(false), force_linear_rollback, synth/invariant/reflect abort
  AC3  Soft/Off observe-only (no TLS write)
  AC4  Existing suites; extend test_coercion_map_abort_rewind
  AC5  Counter on type-linear-evolution-snapshot
  AC6  No test_issue_3116.cpp / no docs/design/3116-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ev = _read("src/compiler/evaluator.ixx")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    cmap = _read("src/compiler/coercion_map.ixx")
    snap = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    test = _read("tests/compiler/test_coercion_map_abort_rewind.cpp")
    build = _read("build.py")

    must("dual_clear_coercion_state_on_abort", "AC1 decl", ev)
    must("take_coercions()", "AC1 take last_coercions_", bound)
    must("clear_coercion_active_mutation_context()", "AC1 TLS clear", bound)
    must("g_coercion_abort_dual_clear_total", "AC1 counter", cmap)
    must("3116 AC1", "AC1 test", test)

    must("dual_clear_coercion_state_on_abort()", "AC2 exit(false)", bound)
    must("dual_clear_coercion_state_on_abort()", "AC2 force_linear_rollback", tc)
    if bound.count("dual_clear_coercion_state_on_abort()") < 4:
        fails.append("AC2: expected ≥4 dual_clear call sites in mutation_boundary")
    must("3116 AC3", "AC3 Soft test", test)

    must("g_coercion_abort_dual_clear_observe_total", "AC3 observe counter", cmap)
    must("observe counter only", "AC3 Soft comment", bound)

    must("3116", "AC4 extend rewind suite", test)
    must("check_coercion_abort_dual_clear_3116", "AC4 build.py", build)

    must("coercion-abort-dual-clear-total", "AC5 snapshot key", snap)
    must("schema-3116", "AC5 schema", snap)

    if (ROOT / "tests" / "compiler" / "test_issue_3116.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_3116.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3116-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3116 coercion abort dual-clear — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
