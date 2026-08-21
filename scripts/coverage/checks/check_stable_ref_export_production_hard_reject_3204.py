#!/usr/bin/env python3
"""Issue #3204: production Agent export hard-reject tenant_id=0 / unrefreshable.

#2404 hard-reject was env-only; production defaults did not arm it, so
layout-only tenant_id==0 StableNodeRef could reach Agent mailbox/handoff.
Arm via apply_production_security_defaults (no env). Stamp via
Evaluator::stamp_stable_ref (#2759). Soft/Off pref false. Reuses
stable_ref_export_stale_reject_total. No new query key.

Contract:
  AC1 production defaults arm hard-reject without env
  AC2 handoff/export never delivers tenant_id==0 under isolation
  AC3 Soft/Off unchanged; quiet tenant already stamped
  AC4 extend test_tenant_isolation_enforcement; this linter; no invent / docs

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

    prov = _read("src/core/provenance_tracker.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    dfl = _read("src/compiler/security_defaults.hh")
    ev = _read("src/compiler/evaluator.ixx")
    t = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp") + _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    build = _read("build.py")

    must("kStableRefExportProductionHardRejectIssue = 3204", "AC1 stamp", prov)
    must("set_stable_ref_export_hard_reject", "AC1 pref", prov)
    must("set_stable_ref_export_hard_reject", "AC1 defaults", dfl)
    must("Issue #3204", "AC1 defaults cite", dfl)
    must("ac3204_1_production_hard_reject", "AC1 test", t)

    must("Issue #3204", "AC2 finalize", sec)
    must("stamp_stable_ref", "AC2 stamp authority", sec)
    must("handoff_ref", "AC2 handoff", sec)
    must("ac3204_2_handoff_never_tenant_zero", "AC2 test", t)
    must("record_stable_ref_export_stale_reject", "AC2 metric", sec)

    must("ac3204_3_soft_quiet", "AC3 test", t)
    must("if (ref.tenant_id == 0 &&", "AC3 quiet skip", sec)
    h = prov.find("inline bool stable_ref_export_hard_reject()")
    if h < 0:
        fails.append("AC3: stable_ref_export_hard_reject missing")

    must("stamp_stable_ref", "AC4 #2759", sec)
    must("check_stable_ref_export_production_hard_reject_3204", "AC4 build.py", build)
    must("ac3204_4_concurrent", "AC4 chaos", t)
    must("ac3204_5_source_linter", "AC4 test", t)
    must("Issue #3204", "AC4 export_ref comment", ev)
    if "schema-3204" in q:
        fails.append("AC4: new schema-3204 query key")
    if "g_3204_" in sec or "g_3204_" in prov:
        fails.append("AC4: new g_3204_* counter")
    if (ROOT / "tests" / "core" / "test_issue_3204.cpp").is_file():
        fails.append("AC4: tests/core/test_issue_3204.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3204.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3204.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3204-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3204 stable_ref_export_production_hard_reject:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3204 stable_ref_export_production_hard_reject: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
