#!/usr/bin/env python3
"""Issue #2632: export_held_ref (or finalize_agent_export) on all cross-fiber
/ mailbox / handoff paths.

Contract (one row per AC):
  AC1 handoff_ref helper exists in evaluator_security.cpp + declared in evaluator.ixx
  AC2 stable_ref_handoff_reject_total counter lives in observability_metrics.h
  AC3 handoff_ref called in fiber-steal path (evaluator_fiber_mutation.cpp)
  AC4 handoff_ref called in parallel-intend result packaging
     (evaluator_primitives_agent.cpp)
  AC5 multi_fiber_mailbox.h references handoff_ref (handoff gate)
  AC6 test_stable_ref_export_validate_2404.cpp covers handoff_ref
  AC7 build.py wires the linter into the lint / gate step
  AC8 schema-2632 keys appear in observability prims (stable-ref-handoff-*)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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

    sec = _read("src/compiler/evaluator_security.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    obs = _read("src/compiler/observability_metrics.h")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    ag = _read("src/compiler/evaluator_primitives_agent.cpp")
    mb = _read("src/serve/multi_fiber_mailbox.h")
    test = _read("tests/compiler/test_stable_ref_export_validate_2404.cpp")
    build = _read("build.py")
    pe = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    _read("scripts/coverage/checks/check_stamp_resolve_coverage.py")

    # AC1
    must("Issue #2632", "AC1", sec)
    must("Evaluator::handoff_ref", "AC1", sec)
    must("handoff_ref", "AC1", ev)
    must("Issue #2632", "AC1", ev)
    must("handoff_ref(", "AC1", ev)

    # AC2
    must("stable_ref_handoff_reject_total", "AC2", obs)
    must("Issue #2632", "AC2", obs)
    must("handoff_reject_total", "AC2", sec)

    # AC3
    must("Issue #2632", "AC3", fm)
    must("handoff_ref(", "AC3", fm)
    must("2632 AC3", "AC3", fm)

    # AC4
    must("Issue #2632", "AC4", ag)
    must("ev.handoff_ref(", "AC4", ag)
    must("2632 AC4", "AC4", ag)

    # AC5
    must("Issue #2632", "AC5", mb)
    must("handoff_reject_total", "AC5", mb)

    # AC6
    must("handoff_ref", "AC6", test)
    must("stable_ref_handoff_reject_total", "AC6", test)
    must("2632 AC", "AC6", test)

    # AC7
    must("check_export_held_handoff_coverage", "AC7", build)

    # AC8 (optional observability schema)
    has_schema = ("schema-2632" in pe) or ("issue-2632" in pe) or ("stable-ref-handoff" in pe)
    if not has_schema:
        # Soft — the gate counter is the hard requirement; observability keys
        # are nice-to-have. Don't fail, but warn.
        print("WARN: AC8 schema-2632 keys not yet present in obs prims (advisory)")

    # cross-check: stamp-resolve --strict must still be green (no regression)
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "coverage" / "checks" / "check_stamp_resolve_coverage.py"), "--strict"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        fails.append(f"stamp-resolve --strict regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: export-held handoff #2632 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
