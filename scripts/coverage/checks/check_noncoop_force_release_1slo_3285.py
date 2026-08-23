#!/usr/bin/env python3
"""Issue #3285 linter — 1×SLO synthetic-edge inject tier for non-cooperative
outermost MutationBoundary holders (I1 residual of #3254/#3222).

Residual: a non-cooperative outermost MutationBoundaryGuard body that never
hits check_gc_safepoint / cooperative yield / Phase-5 could keep
workspace_mtx_ + per-fiber depth slot held past the hold-budget inbody
window. #3254 injects a synthetic MutationBoundary yield and consumes it on
the holder; but the watchdog only escalated at the 2×SLO hard bound, so a
body whose next cooperative edge lands inside [1×SLO, 2×SLO) was not nudged
early enough. This adds a 1×SLO inject tier: as soon as the cancel has been
armed for ≥1×SLO (production), inject the synthetic edge same-fiber or set
the cross-fiber urgent inbody poll (#3223 helper) so the victim's next edge
force-releases (dual-restore + unlock + depth 0) within the 2×SLO window.
Cross-fiber never drops the unique_lock from the foreign thread (AC2).

Gate rows:
  G1  fiber.cpp cites Issue #3285 in aura_hold_budget_poll_inbody_window.
  G2  1×SLO tier keyed on mutation_hold_slo_us + elapsed_us > slo_us.
  G3  Soft gate preserved (mutation_hold_budget_reject_enabled).
  G4  same-fiber inject OR cross-fiber urgent-inbody-poll nudge present.
  G5  2×SLO hard bound + inbody_window_exceeded_total preserved (force path).
  G6  counter reuse — no new keys (forced_unlock_total /
      forced_fail_closed_total / inbody_window_exceeded_total).
  G7  test ACs in tests/serve/test_hold_budget_synthetic_yield_injection.cpp
      (#3133/#3254 suite home, #81967) citing #3285.
  G8  build.py wires this linter.
  G9  no docs/design/3285-* (per #1655), no tests/issue*/test_issue_3285.cpp
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
    print("=== #3285 1×SLO inject tier linter ===")
    fc = read("src/serve/fiber.cpp")
    mh = read("src/compiler/mutation_hold_budget.h")
    test = read("tests/serve/test_hold_budget_synthetic_yield_injection.cpp")
    build = read("build.py")

    pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept")
    must(pos != -1, "G0: poll definition present")
    win = fc[pos : pos + 3200] if pos != -1 else ""

    must("Issue #3285" in win, "G1: fiber.cpp poll cites Issue #3285")
    must(
        "mutation_hold_slo_us()" in win and "elapsed_us > slo_us" in win, "G2: 1×SLO tier keyed on mutation_hold_slo_us"
    )
    must("mutation_hold_budget_reject_enabled()" in win, "G3: Soft gate via reject_enabled preserved")
    must(
        "inject_synthetic_mutation_boundary_yield()" in win or "aura_fiber_request_urgent_inbody_poll" in win,
        "G4: same-fiber inject OR cross-fiber urgent poll nudge",
    )
    must("inbody_window_exceeded_total" in win, "G5: 2×SLO hard-bound force path preserved")
    must(
        "g_mutation_hold_budget_forced_unlock_total" in mh
        and "g_mutation_hold_budget_forced_fail_closed_total" in mh
        and "g_mutation_hold_budget_inbody_window_exceeded_total" in mh,
        "G6: counter reuse — no new keys",
    )
    must(
        "run_test_hold_budget_1slo_inject_3285" in test and "Issue #3285" in test,
        "G7: test ACs in #3133/#3254 suite home cite #3285",
    )
    must("check_noncoop_force_release_1slo_3285.py" in build, "G8: build.py wires linter")

    docs_ok = True
    if (ROOT / "docs/design").exists():
        docs_ok = not any(p.name.startswith("3285-") for p in (ROOT / "docs/design").glob("3285-*"))
    must(docs_ok, "G9a: no docs/design/3285-* per #1655")
    must(
        not (ROOT / "tests" / "issues" / "test_issue_3285.cpp").exists(),
        "G9b: no tests/issues/test_issue_3285.cpp per #81967",
    )

    print()
    if failures:
        print(f"#3285 linter FAILED: {len(failures)} gate(s) — {failures}")
        return 1
    print("#3285 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
