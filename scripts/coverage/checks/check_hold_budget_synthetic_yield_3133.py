#!/usr/bin/env python3
"""Issue #3133: P0 residual — synthetic YieldReason::MutationBoundary
injection on non-yielding hold-budget holders. Closes the #3071/#3035
residual window where a body that never hits check_gc_safepoint / yield
keeps the exclusive lock after cancel is armed.

Contract:
  AC1 fiber.h + fiber.cpp — Fiber::inject_synthetic_mutation_boundary_yield()
     sets force_safepoint_requested_ AND last_yield_reason_ = MutationBoundary
     synthetically. Poll caller replaces request_force_safepoint() with
     inject() on subsequent polls (first escalation via
     aura_evaluator_force_degrade_outermost_holder preserved).
  AC2 Soft/sandbox=off contract preserved — mutation_hold_budget_reject_
     enabled() gate; Soft: no inject call (metric-only).
  AC3 Nested Guards never independently force-fail — outermost-only contract
     preserved (#2932 / #2999 / #3035).
  AC4 Existing counters reused (g_mutation_hold_budget_forced_fail_closed_
     total #3035 + g_mutation_hold_budget_inbody_window_exceeded_total
     #3071). No new query key.
  AC5 Regression test in tests/serve/ (src/-aligned per #81967). No
     tests/issues/test_issue_3133.cpp. No docs/design/3133-* (#1655).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _window(src: str, start_token: str, end_token: str) -> str:
    """Return substring of src between start_token and end_token (exclusive)."""
    a = src.find(start_token)
    if a == -1:
        return ""
    b = src.find(end_token, a + len(start_token))
    if b == -1:
        return src[a : a + 8000]
    return src[a:b]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    fh = _read("src/serve/fiber.h")
    fc = _read("src/serve/fiber.cpp")
    efl = _read("src/compiler/evaluator_fiber_mutation.cpp")
    mhb = _read("src/compiler/mutation_hold_budget.h")
    test = _read("tests/serve/test_hold_budget_synthetic_yield_injection.cpp")
    q = read_query_prims()

    # ── AC1: method declared in fiber.h + implemented in fiber.cpp ──
    method_pos = fh.find("inject_synthetic_mutation_boundary_yield()")
    if method_pos == -1:
        fails.append("AC1: Fiber::inject_synthetic_mutation_boundary_yield not declared")
    else:
        # Method block: extend the window BACKWARDS to include the
        # comment block (the comment is BEFORE the method declaration
        # in fiber.h, so anchoring on the method name misses it).
        method_start = max(0, method_pos - 1500)
        method_end = fh.find("};", method_pos)
        if method_end == -1 or method_end > method_pos + 1500:
            method_end = method_pos + 1500
        method_win = fh[method_start:method_end]
        must("Issue #3133", "AC1 declaration cites #3133", method_win)
        must("force_safepoint_requested_.store(true", "AC1 sets force_safepoint_requested_", method_win)
        must(
            "last_yield_reason_.store(YieldReason::MutationBoundary",
            "AC1 sets last_yield_reason_ synthetically",
            method_win,
        )
        must("mutation_hold_budget_reject_enabled", "AC1 comments reference Soft-gate contract", method_win)

    impl_pos = fc.find("void Fiber::inject_synthetic_mutation_boundary_yield()")
    if impl_pos == -1:
        fails.append("AC1: method implementation missing in fiber.cpp")
    else:
        # Extend backwards to include the impl comment block.
        impl_start = max(0, impl_pos - 1500)
        impl_end = fc.find("}\n", impl_pos + 200)
        if impl_end == -1 or impl_end > impl_pos + 1200:
            impl_end = impl_pos + 1200
        impl_win = fc[impl_start:impl_end]
        must("force_safepoint_requested_.store(true", "AC1 impl sets force_safepoint_requested_", impl_win)
        must("last_yield_reason_.store(YieldReason::MutationBoundary", "AC1 impl sets last_yield_reason_", impl_win)

    # ── AC1: poll caller wires through to inject ──
    poll_anchor = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept")
    if poll_anchor == -1:
        fails.append("AC1: poll definition missing")
    else:
        poll_end = poll_anchor + 6000
        poll_win = fc[poll_anchor:poll_end]
        must("Issue #3133", "AC1 poll cites #3133", poll_win)
        must("inject_synthetic_mutation_boundary_yield()", "AC1 poll calls inject on subsequent polls", poll_win)
        must(
            "aura_evaluator_force_degrade_outermost_holder(fid)",
            "AC1 first escalation via force_degrade preserved",
            poll_win,
        )

    # ── AC2: Soft gate preserved ──
    if poll_anchor != -1:
        must("mutation_hold_budget_reject_enabled()", "AC2 Soft gate via reject_enabled", poll_win)
        must("if (!mutation_hold_budget_reject_enabled())", "AC2 Soft early-return gate", poll_win)
        must("return 0; // Soft / sandbox=off", "AC2 Soft path metric-only", poll_win)

    # ── AC3: outermost-only contract preserved ──
    fd_anchor = efl.find("aura_evaluator_force_degrade_outermost_holder(std::uint64_t fiber_id)")
    if fd_anchor == -1:
        fails.append("AC3: force_degrade_outermost_holder definition missing")
    else:
        fd_end = fd_anchor + 4000
        fd_win = efl[fd_anchor:fd_end]
        must("mark_outermost_mutation_failed", "AC3 outermost-only mark preserved", fd_win)
        must("outermost", "AC3 outermost contract preserved", fd_win)

    # ── AC4: existing counters reused; no new query key ──
    must(
        "g_mutation_hold_budget_forced_fail_closed_total", "AC4 existing #3035 forced_fail_closed_total preserved", mhb
    )
    must(
        "g_mutation_hold_budget_inbody_window_exceeded_total",
        "AC4 existing #3071 inbody_window_exceeded_total preserved",
        mhb,
    )
    if poll_anchor != -1:
        must(
            "g_mutation_hold_budget_inbody_window_exceeded_total.fetch_add",
            "AC4 existing counter bumped (poll)",
            poll_win,
        )
    if "schema-3133" in q:
        fails.append("AC4: new query key schema-3133 (forbidden)")

    # ── AC5: src-aligned test, no tests/issues/test_issue_3133.cpp, no plan doc ──
    must("Issue #3133", "AC5 regression test cites", test)
    must("inject_synthetic_mutation_boundary_yield", "AC5 regression test asserts the inject method", test)
    if (ROOT / "tests" / "issues" / "test_issue_3133.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3133.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "serve" / "test_issue_3133.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_3133.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3133-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    # Coverage wiring — linter must be reachable from build.py.
    must("check_hold_budget_synthetic_yield_3133", "AC6 build.py wiring", _read("build.py") + _read("pyproject.toml"))

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3133 hold-budget synthetic yield injection — all 5 AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
