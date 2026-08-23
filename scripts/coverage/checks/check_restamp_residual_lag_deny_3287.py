#!/usr/bin/env python3
"""Issue #3287 linter — outermost restamp over-budget residual tear must not
leave a clean StableNodeRef / query:*-stable export face on lagging nodes.

Residual (I6): the production outermost success path degrades restamp_all to
lazy-align when over budget, then eager-restamps only the hot cone (#3259,
dirty roots + parent chain up to restamp_hot_cone_budget = budget/2). The
non-hot remainder stays torn / restamp-lag. Required close #1: after the
hot-cone eager restamp, if any remaining nodes are still lagging the
authority generation, production must mark those faces stale (deny clean
hit) — never export a pre-mutate gen that looks green.

Fix:
- query:stable-ref-provenance (evaluator_primitives_query.cpp) now consults
  allow_query_stable_ref_export(nid) under production before capturing — a
  lagging node returns deny (no clean export).
- unified_restamp_after_boundary (evaluator_fiber_mutation.cpp) adds a
  residual-lag assertion after the hot cone: if still restamp_over_budget_torn()
  under production, bump the existing g_unified_restamp_torn_visible_total
  (no new metric key) so Agents / CI can assert fail-closed.

Gate rows:
  G1  evaluator_primitives_query.cpp cites Issue #3287 on the
      query:stable-ref-provenance surface.
  G2  that surface consults allow_query_stable_ref_export under production
      (deny clean hit).
  G3  evaluator_fiber_mutation.cpp cites Issue #3287 residual-lag assertion
      after the hot cone.
  G4  the assertion checks restamp_over_budget_torn() and bumps the existing
      g_unified_restamp_torn_visible_total (no new key).
  G5  test ACs in tests/core/test_stable_ref_tenant_capture.cpp (#3259 suite
      home, #81967) citing #3287.
  G6  build.py wires this linter.
  G7  no docs/design/3287-* (per #1655), no tests/issue*/test_issue_3287.cpp
      (per #81967).

Exit 0 = all rows satisfied.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

failures: list[str] = []


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def read(rel: str) -> str:
    p = ROOT / rel
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def main() -> int:
    print("=== #3287 restamp residual-lag deny linter ===")
    q = read("src/compiler/evaluator_primitives_query.cpp")
    fm = read("src/compiler/evaluator_fiber_mutation.cpp")
    test = read("tests/core/test_stable_ref_tenant_capture.cpp")
    build = read("build.py")

    qpos = q.find('add("query:stable-ref-provenance"')
    if qpos == -1:
        qpos = q.find("query:stable-ref-provenance", q.find("query:stable-ref-provenance") + 1)
    must(qpos != -1, "G0: query:stable-ref-provenance surface present")
    qwin = q[qpos : qpos + 2600] if qpos != -1 else ""
    must("Issue #3287" in qwin, "G1: stable-ref-provenance cites Issue #3287")
    must(
        "allow_query_stable_ref_export" in qwin and "production_defaults_active()" in qwin,
        "G2: surface consults torn gate under production (deny clean hit)",
    )

    fpos = fm.find("Issue #3287: residual-lag assertion")
    must(fpos != -1, "G3: boundary cites Issue #3287 residual-lag assertion")
    fwin = fm[fpos : fpos + 1000] if fpos != -1 else ""
    must("restamp_over_budget_torn()" in fwin, "G4a: residual tear check after hot cone")
    must("g_unified_restamp_torn_visible_total" in fwin, "G4b: reuses existing torn-visible bus (no new key)")

    must(
        "ac3287_1_residual_lag_deny_surface" in test and "Issue #3287" in test,
        "G5: test ACs in #3259 suite home cite #3287",
    )
    must("check_restamp_residual_lag_deny_3287.py" in build, "G6: build.py wires linter")

    docs_ok = True
    if (ROOT / "docs/design").exists():
        docs_ok = not any(p.name.startswith("3287-") for p in (ROOT / "docs/design").glob("3287-*"))
    must(docs_ok, "G7a: no docs/design/3287-* per #1655")
    must(
        not (ROOT / "tests" / "issues" / "test_issue_3287.cpp").exists(),
        "G7b: no tests/issues/test_issue_3287.cpp per #81967",
    )

    print()
    if failures:
        print(f"#3287 linter FAILED: {len(failures)} gate(s) — {failures}")
        return 1
    print("#3287 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
