#!/usr/bin/env python3
"""Issue #3289 linter — mailbox under-boundary wait p99 must guarantee
progress for an already-held outermost Guard (I5 residual of #3285).

Residual: mailbox under-boundary SLO breach armed the hold-budget cancel
once (force-degrade + request_hold_budget_cancel) and polled the in-body
window at arm time — but elapsed≈0 there, so the #3285 1×SLO synthetic-edge
tier never fired. A non-cooperative (or slow-coop) holder that never
reaches its own cooperative edge was never re-polled by the mailbox path
(the scheduler idle-tick poll is not a hard progress bound under load) →
prolonged half-green: workspace held, new mutates denied, steal starved,
GC defer armed, without a machine-guaranteed progress bound for the holder.

Fix: maybe_mailbox_defer_slo_hold_cancel's already-armed branch now
re-polls aura_hold_budget_poll_inbody_window() — the exact same force path
as hold-budget overtime (1×SLO synthetic-edge inject → 2×SLO hard bound →
force-release: depth 0 + unlocked + dual restore). No new counters, no
second unlock path, Soft observe-only unchanged.

Gate rows:
  G1  multi_fiber_mailbox.h cites Issue #3289 on the armed branch and
      re-polls aura_hold_budget_poll_inbody_window().
  G2  mailbox arm path calls the same force-degrade / inject ABIs as
      hold-budget overtime (aura_evaluator_force_degrade_outermost_holder
      + aura_fiber_request_hold_budget_cancel).
  G3  Soft observe-only preserved (mailbox_defer_slo_soft_observe_total +
      production_defaults gate).
  G4  no new counters (no g_3289_*).
  G5  test AC in src-aligned suite (#81967): mailbox starvation suite
      ac3289_1; no tests/issue*/test_issue_3289.cpp.
  G6  no docs/design/ (#1655).
  G7  build.py wires this linter.

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
    mfh = read("src/serve/multi_fiber_mailbox.h")
    read("src/serve/fiber.cpp")
    t_mbox = read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    build = read("build.py")

    # ── G1: armed-branch re-poll cites #3289 ──
    must("Issue #3289" in mfh, "G1: multi_fiber_mailbox.h cites Issue #3289")
    # Anchor at the CAS usage (not the declaration at the top of the file)
    # so the window covers the armed-branch re-poll below it.
    arm_pos = mfh.find("g_mailbox_defer_slo_hold_cancel_armed.compare_exchange_strong")
    must(arm_pos >= 0, "G1: armed CAS present")
    if arm_pos >= 0:
        win = mfh[arm_pos : arm_pos + 2600]
        must("aura_hold_budget_poll_inbody_window()" in win, "G1: armed branch re-polls the in-body window")
        must("force-release as hold-budget" in win, "G1: reuses the same force-release path as hold-budget overtime")

    # ── G2: same force-degrade / inject ABIs ──
    must(
        "aura_evaluator_force_degrade_outermost_holder" in mfh,
        "G2: mailbox arm calls force-degrade (same ABI as hold-budget)",
    )
    must("aura_fiber_request_hold_budget_cancel" in mfh, "G2: mailbox arm calls hold-budget cancel (same ABI)")

    # ── G3: Soft observe-only preserved ──
    must("mailbox_defer_slo_soft_observe_total" in mfh, "G3: Soft observe counter preserved")
    must("aura_production_defaults_active_probe() == 0" in mfh, "G3: Soft gate preserved (observe-only)")

    # ── G4: no new counters ──
    must("g_3289_" not in mfh, "G4: no new g_3289_* counter in multi_fiber_mailbox.h")

    # ── G5: src-aligned suite home (#81967) ──
    must("ac3289_1_mailbox_slo_repoll_source" in t_mbox, "G5: mailbox starvation suite AC (#3289)")
    must(
        "ac3289_1_mailbox_slo_repoll_source();" in t_mbox, "G5: AC registered in run_test_mailbox_hold_starvation_hard"
    )
    must(not read("tests/issues/test_issue_3289.cpp"), "G5: no tests/issues/test_issue_3289.cpp per #81967")
    must(not read("tests/serve/test_issue_3289.cpp"), "G5: no tests/serve/test_issue_3289.cpp per #81967")

    # ── G6: no docs/design ──
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        bad = [f.name for f in sorted(docs.glob("3289-*"))]
        must(not bad, "G6: no docs/design/3289-* per #1655")
    else:
        must(True, "G6: no docs/design/3289-* per #1655")

    # ── G7: build.py wiring ──
    must("check_under_boundary_hold_progress_3289" in build, "G7: build.py wires linter")

    if failures:
        print(f"\n#3289 linter: {len(failures)} gate(s) FAILED")
        return 1
    print("\n#3289 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
