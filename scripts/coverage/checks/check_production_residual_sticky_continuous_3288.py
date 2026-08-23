#!/usr/bin/env python3
"""Issue #3288 linter — production multi-worker Ready residual-zero sticky
must be a continuous fail-closed gate, not query-only.

Residual (I3/I6): residual counters can become non-zero AFTER Ready
(re-arm race, densify×steal, LifetimeProof lag) and the process kept
admitting steals / mutates until an external query polled the sticky bit
(schema-3073 / production-readiness-steal-residual-sticky-fail). Chaos soak
was not a hard Ready gate for continuous residual growth.

Fix:
- steal_safety_transaction Ok path (steal_safety.cpp) now consults
  g_steal_safety_production_residual_sticky_fail under
  aura_runtime_multi_worker_production_latched() BEFORE the ticket stamp —
  sticky set → RejectHard (no ticket, no enqueue) until residual returns to 0.
- MutationBoundaryGuard::try_acquire + try_acquire_for_region
  (evaluator_mutation_boundary.cpp) refuse a new outermost Guard admit with
  structured AdmissionRejected: production-residual-sticky under the same
  latch + sticky.
- Reuses the existing sticky bit + residual counters — no second residual
  bus, no new metric key (schema-3288 / issue-3288 additive stamps only).

Gate rows:
  G1  steal_safety.cpp Ok path cites Issue #3288 and consults
      g_steal_safety_production_residual_sticky_fail.load before
      set_resume_safety_ticket, gated on
      aura_runtime_multi_worker_production_latched() != 0.
  G2  evaluator_mutation_boundary.cpp try_acquire + try_acquire_for_region
      cite Issue #3288 and refuse with
      AdmissionRejected: production-residual-sticky under latch + sticky.
  G3  no new counters (no g_3288_* in serve or compiler TUs).
  G4  query_type_stats.cpp additive schema-3288 / issue-3288 stamps only.
  G5  test ACs in src-aligned suite homes (#81967): residual-zero suite
      AC14 + txn suite ac3288_1/ac3288_2; no tests/issue*/test_issue_3288.cpp.
  G6  no docs/design/3288-* (per #1655).
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
    sc = read("src/serve/steal_safety.cpp")
    sh = read("src/serve/steal_safety.h")
    mb = read("src/compiler/evaluator_mutation_boundary.cpp")
    qts = read("src/compiler/evaluator_primitives_query_type_stats.cpp")
    t_zero = read("tests/serve/test_steal_safety_production_residual_zero.cpp")
    read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = read("build.py")

    # ── G1: Ok-path sticky consult before ticket stamp, latch-gated ──
    must("Issue #3288" in sc, "G1: steal_safety.cpp cites Issue #3288")
    fn_pos = sc.find("StealSafetyDecision steal_safety_transaction(")
    must(fn_pos >= 0, "G1: steal_safety_transaction present")
    if fn_pos >= 0:
        fn_win = sc[fn_pos : fn_pos + 9000]
        load_pos = fn_win.find("g_steal_safety_production_residual_sticky_fail.load")
        stamp_pos = fn_win.find("set_resume_safety_ticket(snap.ticket)")
        must(load_pos >= 0, "G1: Ok path consults sticky bit (.load)")
        must(stamp_pos >= 0, "G1: ticket stamp present")
        if load_pos >= 0 and stamp_pos >= 0:
            must(load_pos < stamp_pos, "G1: sticky consult BEFORE ticket stamp")
            win = fn_win[max(0, load_pos - 300) : load_pos + 300]
            must(
                "aura_runtime_multi_worker_production_latched() != 0" in win,
                "G1: sticky consult gated on multi-worker latch",
            )

    # ── G2: try_acquire + try_acquire_for_region sticky refusal ──
    must("Issue #3288" in mb, "G2: evaluator_mutation_boundary.cpp cites Issue #3288")
    must(
        "AdmissionRejected: production-residual-sticky" in mb,
        "G2: structured AdmissionRejected: production-residual-sticky present",
    )
    must(
        "steal_safety_production_residual_sticky_fail_v_read() != 0" in mb,
        "G2: sticky accessor consulted in try_acquire paths",
    )
    must(
        "aura_runtime_multi_worker_production_latched() != 0" in mb,
        "G2: latch-gated (Soft / single-worker / unlatched unchanged)",
    )
    # Both admit paths (try_acquire + try_acquire_for_region) carry the gate.
    # Anchor on the definitions (Evaluator:: prefix) — a bare
    # "MutationBoundaryGuard::try_acquire(" first matches an internal call
    # site (e.g. line 635), whose window cannot see the gate.
    ta_pos = mb.find("Evaluator::MutationBoundaryGuard::try_acquire(")
    tar_pos = mb.find("Evaluator::MutationBoundaryGuard::try_acquire_for_region(")
    must(ta_pos >= 0, "G2: try_acquire definition present")
    must(tar_pos >= 0, "G2: try_acquire_for_region definition present")
    if ta_pos >= 0:
        must("production-residual-sticky" in mb[ta_pos : ta_pos + 6000], "G2: try_acquire carries the sticky gate")
    if tar_pos >= 0:
        must(
            "production-residual-sticky" in mb[tar_pos : tar_pos + 6000],
            "G2: try_acquire_for_region carries the sticky gate",
        )

    # ── G3: no new counters ──
    for hay, label in ((sc, "steal_safety.cpp"), (sh, "steal_safety.h"), (mb, "mutation_boundary")):
        must("g_3288_" not in hay, f"G3: no new g_3288_* counter in {label}")

    # ── G4: additive schema stamps only ──
    must("schema-3288" in qts, "G4: schema-3288 additive stamp")
    must("issue-3288" in qts, "G4: issue-3288 additive stamp")

    # ── G5: src-aligned suite homes (#81967) ──
    must("AC14 (Issue #3288)" in t_zero, "G5: residual-zero suite AC14 (#3288)")
    must("AC15 (Issue #3288)" in t_zero, "G5: residual-zero suite AC15 live Ok-path gate (#3288)")
    must("steal_safety_transaction(&f)" in t_zero, "G5: live transaction exercised in residual-zero suite")
    must(
        "set_production_multi_worker_latched_for_test(true)" in t_zero,
        "G5: live latch exercised in residual-zero suite",
    )
    must(not read("tests/issues/test_issue_3288.cpp"), "G5: no tests/issues/test_issue_3288.cpp per #81967")
    must(not read("tests/serve/test_issue_3288.cpp"), "G5: no tests/serve/test_issue_3288.cpp per #81967")

    # ── G6: no docs/design ──
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        bad = [f.name for f in sorted(docs.glob("3288-*"))]
        must(not bad, "G6: no docs/design/3288-* per #1655")
    else:
        must(True, "G6: no docs/design/3288-* per #1655")

    # ── G7: build.py wiring ──
    must("check_production_residual_sticky_continuous_3288" in build, "G7: build.py wires linter")

    if failures:
        print(f"\n#3288 linter: {len(failures)} gate(s) FAILED")
        return 1
    print("\n#3288 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
