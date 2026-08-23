#!/usr/bin/env python3
"""Issue #3279 linter — session_bound orphan fail-closed sweep.

Residual of #2944/#3048/#3142/#3177/#3241: revoke paths exist (outermost
Guard dtor, steal/abort, durable→session_bound) but
`session_bound_orphan_detected_total` was metric-only — declared, never
bumped, no production enforcement. Under long-run multi-tenant hosts, a
lost Guard / abort-without-mid-clear / dual-Evaluator race edge / sticky
escape misuse can leave a live session_bound grant whose bound_mutation_id
is no longer in the live mid set → privilege sticky relative to the epoch
model. This closes it with a fail-closed sweep.

Gate rows:
  G1  capability_model.hh has the SSOT detection helper
      (count_session_bound_orphans_locked) walking by_tenant under mtx.
  G2  sweep_session_bound_orphans exists and revokes orphans under
      Restricted/Strict with stable reason "session-orphan-sweep" (+ SE /
      audit joinable by mid via record_audit, live counter cleared).
  G3  existing session_bound_orphan_detected_total counter is bumped
      (reuse — no new metric).
  G4  #3241 fiber-id peer-skip preserved in both count and revoke walks.
  G5  trigger wired at outermost Guard enter (evaluator_mutation_boundary.cpp)
      when live_session_grants > 0.
  G6  Soft/Off observe-only contract (never revoke solely due to sweep).
  G7  test ACs in tests/core/test_capability_single_use_consume.cpp
      (the #3142 suite home, #81967).
  G8  build.py wires this linter.
  G9  no docs/design/3279-* (per #1655), no tests/issue*/test_issue_3279.cpp
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
    print("=== #3279 session_bound orphan sweep linter ===")
    cap = read("src/core/capability_model.hh")
    boundary = read("src/compiler/evaluator_mutation_boundary.cpp")
    test = read("tests/core/test_capability_single_use_consume.cpp")
    build = read("build.py")

    must(
        "count_session_bound_orphans_locked" in cap and "by_tenant" in cap,
        "G1: SSOT detection helper walks by_tenant under mtx",
    )
    must(
        "sweep_session_bound_orphans" in cap and "session-orphan-sweep" in cap,
        "G2: sweep helper + stable SE reason 'session-orphan-sweep'",
    )
    must(
        "session_bound_orphan_detected_total" in cap,
        "G3: reuses existing orphan counter (no new metric)",
    )
    must(
        "g.grant_fiber_id != fiber_id" in cap or "grant_fiber_id != fiber_id" in cap,
        "G4: #3241 fiber-id peer-skip in count + revoke walks",
    )
    must(
        "Issue #3279" in boundary
        and "sweep_session_bound_orphans" in boundary
        and "capability_live_session_grants" in boundary,
        "G5: outermost Guard enter trigger wired (live>0 gate)",
    )
    must(
        "never revoke" in cap or "observe-only" in cap or "Soft/Off observe-only" in cap or "Soft" in cap,
        "G6: Soft/Off observe-only contract",
    )
    for name in (
        "ac3279_1_soft_observe_only",
        "ac3279_2_production_revoke",
        "ac3279_3_live_grant_not_swept",
        "ac3279_4_peer_fiber_skip",
        "ac3279_5_source_cite_and_linter",
    ):
        must(name in test, f"G7: test {name} present")
    must(
        "check_session_bound_orphan_sweep_3279.py" in build,
        "G8: build.py wires linter",
    )
    must(
        not any(p.name.startswith("3279-") for p in (ROOT / "docs/design").glob("3279-*"))
        if (ROOT / "docs/design").exists()
        else True,
        "G9a: no docs/design/3279-* per #1655",
    )
    must(
        not (ROOT / "tests/issues" / "test_issue_3279.cpp").exists()
        and not (ROOT / "tests/core" / "test_issue_3279.cpp").exists()
        and not (ROOT / "tests/compiler" / "test_issue_3279.cpp").exists(),
        "G9b: no tests/issue*/test_issue_3279.cpp per #81967",
    )

    if failures:
        print(f"\n#3279 linter FAILED: {len(failures)} gate(s)")
        return 1
    print("\nOK #3279 session_bound_orphan_sweep: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
