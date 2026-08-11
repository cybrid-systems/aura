#!/usr/bin/env python3
"""Issue #2892: Converge post-compact restamp / ownership / EnvFrame scan
into the single post_compact_lifecycle entry (eliminate call-site order
drift).

Post-densify work (pin restamp, EnvFrame densify ownership scan,
ownership_rebind, residual defer audit, LayoutStamp publish) is
scattered across MutationBoundaryGuard Phase-5, evaluator_gc
compact_sweep helpers, and arena live_compact exit, causing order
drift. #2892 completes the convergence on top of the #2436
post_compact_lifecycle scaffold:
  AC1 all densify success paths route through the single
      run_post_compact_close entry (Phase-5 outermost BoundaryGuard
      success); outside restamp/scan/rebind call sites stay inside the
      densify pairing (steps 1-6, pre-orchestrator by design)
  AC2 order fixed + documented (steps 1-10 in post_compact_lifecycle.hh);
      injected out-of-order test FAILs under production
  AC3 soft / empty densify -> zero extra work (soft_skip only)
  AC4 additive metrics only; existing #2340/#2361/#2854/#2837 surfaces
      preserved; single post_compact_lifecycle_ran_total counter added
  AC5 source-cite + extend densify/ownership/EnvFrame suites per #81967;
      no docs/design per #1655

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

    hh = _read("src/core/post_compact_lifecycle.hh")
    ev = _read("src/compiler/evaluator_mutation_boundary.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_densify_ownership_scan_fail_gate.cpp")
    tst2 = _read("tests/core/test_general_object_pin_coverage_gate.cpp")
    build = _read("build.py")

    # AC1 — single entry: run_post_compact_close has exactly one call
    # site (Phase-5 outermost BoundaryGuard success) and it is not
    # duplicated elsewhere.
    must("run_post_compact_close", "AC1", hh)
    must("Issue #2892", "AC1", ev)
    must("(void)run_post_compact_close(close_in, close_hooks)", "AC1", ev)
    if ev.count("(void)run_post_compact_close(close_in, close_hooks)") != 1:
        fails.append(
            "AC1: run_post_compact_close must have exactly one call site "
            "(found " + str(ev.count("(void)run_post_compact_close(close_in, close_hooks)")) + ")"
        )

    # AC4 — single observability counter exists, is documented as the
    # #2892 additive counter, and is bumped on the full-run path.
    must("post_compact_lifecycle_ran_total", "AC4", hh)
    must("Issue #2892: single observability counter", "AC4", hh)
    must("note_lifecycle_ran()", "AC4", hh)
    # Bumped only inside run_post_compact_close full-run path (same
    # condition as note_lifecycle_run), not at arbitrary call sites.
    if hh.count("note_lifecycle_ran()") != 2:
        fails.append(
            "AC4: note_lifecycle_ran() must be defined + called exactly "
            "once (full-run path); found " + str(hh.count("note_lifecycle_ran()"))
        )
    # Additive: existing site-tagged counters preserved.
    for c in (
        "post_compact_lifecycle_runs_total",
        "post_compact_lifecycle_soft_skip_total",
        "post_compact_lifecycle_ir_sync_total",
        "post_compact_lifecycle_stamp_publish_total",
    ):
        must(c, "AC4", hh)

    # AC4 — query surface exposes the new counter.
    must("post-compact-lifecycle-ran-total", "AC4", obs)
    must("post_compact_lifecycle_ran_total.load", "AC4", obs)

    # AC2 — order documented in the scaffold header (steps 1-10).
    must("kStepRootRemap", "AC2", hh)
    must("kStepEnvframe", "AC2", hh)
    must("kStepClosure", "AC2", hh)
    must("kStepLayoutStampPublish", "AC2", hh)
    must("kStepFiberSafe", "AC2", hh)

    # AC3 — soft path only bumps soft_skip (zero extra work).
    must("note_lifecycle_soft_skip()", "AC3", hh)
    if "note_lifecycle_ran()" in hh.split("note_lifecycle_soft_skip()")[0]:
        fails.append("AC3: ran_total must be bumped after soft_skip decision")

    # AC5 — src/-aligned densify/ownership suite extended per #81967
    # (no tests/issues/ file), source-cites present, no docs/design.
    must("Issue #2892", "AC5", test)
    for fn in (
        "ac2892_1_single_entry",
        "ac2892_2_order_fixed_and_documented",
        "ac2892_3_soft_zero_work",
        "ac2892_4_full_run_bumps_ran",
        "ac2892_5_source_and_linter",
    ):
        must(fn, "AC5", test)
    must("Issue #2892", "AC5", tst2)
    must("check_post_compact_lifecycle_2892.py", "AC5", build)
    if _read("tests/issues/test_issue_2892.cpp"):
        fails.append("AC5: no tests/issues/test_issue_2892.cpp (per #81967)")
    if _read("docs/design/2892-post-compact-lifecycle.md"):
        fails.append("AC5: no docs/design/2892-* (per #1655)")

    if fails:
        print("check_post_compact_lifecycle_2892.py: FAIL")
        for f in fails:
            print("  - " + f)
        return 1
    print("check_post_compact_lifecycle_2892.py: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
