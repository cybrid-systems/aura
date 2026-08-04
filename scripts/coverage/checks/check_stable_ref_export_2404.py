#!/usr/bin/env python3
"""Issue #2404: Agent StableNodeRef export validate_or_refresh contract.

Contract:
  AC1 export_ref / finalize_agent_export / ensure-ref / parent-stable wired
  AC2 export-stale-reject counter + hard-reject env
  AC3 already-valid soft path (export-valid)
  AC4 schema-2404 keys on stable-ref-provenance-stats; stamp-resolve green
  AC5 multi-mutate hold + tests + build gate

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

    prov = _read("src/core/provenance_tracker.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    ws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    stamp = _read("scripts/coverage/checks/check_stamp_resolve_coverage.py")
    test = _read("tests/compiler/test_stable_ref_export_validate_2404.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2404", "AC1", sec)
    must("finalize_agent_export", "AC1", sec)
    must("export_held_ref", "AC1", sec)
    must("finalize_agent_export", "AC1", ev)
    must("export_held_ref", "AC1", ev)
    must("export_ref_safe", "AC1", ws)
    must("query:ensure-ref", "AC1", ws)
    must("2404 AC1", "AC1", test)
    must("EXPORT_VALIDATE_HELPERS", "AC1", stamp)
    must("EXPORT_RETURN_PRIMS", "AC1", stamp)

    # AC2
    must("stable_ref_export_stale_reject_total", "AC2", prov)
    must("record_stable_ref_export_stale_reject", "AC2", prov)
    must("AURA_STABLE_REF_EXPORT_HARD_REJECT", "AC2", prov)
    must("stable_ref_export_hard_reject", "AC2", prov)
    must("2404 AC2", "AC2", test)

    # AC3
    must("record_stable_ref_export_valid", "AC3", prov)
    must("already_valid", "AC3", sec)
    must("2404 AC3", "AC3", test)

    # AC4
    must("schema-2404", "AC4", obs)
    must("issue-2404", "AC4", obs)
    must("stable-ref-export-wired", "AC4", obs)
    must("stable-ref-export-refresh-total", "AC4", obs)
    must("stable-ref-export-stale-reject-total", "AC4", obs)
    must("kStableRefExportValidateIssue", "AC4", prov)
    must("2404 AC4", "AC4", test)

    # AC5
    must("2404 AC5", "AC5", test)
    must("check_stable_ref_export_2404", "AC5", build)
    must("cmd_stable_ref_export_coverage", "AC5", build)
    must("test_stable_ref_export_validate_2404", "AC5", cmake)

    # stamp-resolve --strict must pass
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "coverage" / "checks" / "check_stamp_resolve_coverage.py"), "--strict"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        fails.append(f"AC4 stamp-resolve --strict failed:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: stable-ref export validate #2404 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
