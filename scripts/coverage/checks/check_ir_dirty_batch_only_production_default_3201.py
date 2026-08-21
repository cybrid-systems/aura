#!/usr/bin/env python3
"""Issue #3201: production residual single-mark cascades hard-abort by default.

AURA_IR_DIRTY_BATCH_ONLY was opt-in (env=1). Production / Full pack now
default-on via production_defaults probe when env is unset. env=0 remains
Soft/unit residual metric-only. Quiet batch path never calls the probe.

Contract:
  AC1 production_defaults + unset env → ir_dirty_batch_only_hard; abort path
  AC2 Soft / env=0 / unit residual still metric-only
  AC3 batch APIs still clear residual
  AC4 extend test_batch_dirty_discipline; inject residual under production
  AC5 this linter + 3105 abort window preserved
  AC6 no docs/design / test_issue_3201.cpp / new query:*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    soa = _read("src/compiler/ir_soa.ixx")
    t = _read("tests/compiler/test_batch_dirty_discipline.cpp")
    build = _read("build.py")

    must("kIrSoaBatchOnlyProductionDefaultIssue = 3201", "AC1 stamp", soa)
    must("aura_production_defaults_active_probe", "AC1 probe", soa)
    must("Issue #3201", "AC1 cite", soa)
    probe = re.search(
        r"inline bool ir_dirty_batch_only_hard\(\)[^{]*\{(?P<body>.*?)\n\}",
        soa,
        re.DOTALL,
    )
    if not probe:
        fails.append("AC1: could not extract ir_dirty_batch_only_hard")
    else:
        body = probe.group("body")
        if "e[0] == '0'" not in body:
            fails.append("AC2: env=0 force-off missing")
        if "e[0] == '1'" not in body:
            fails.append("AC1: env=1 force-on missing")
        if "aura_production_defaults_active_probe" not in body:
            fails.append("AC1: unset env must consult production probe")

    abort_window = re.search(
        r"if \(ir_dirty_batch_only_hard\(\)\)\s*\{[^}]{0,2000}?g_ir_soa_batch_only_hard_abort_total\.fetch_add\("
        r"[^}]{0,2000}?std::abort\(\)",
        soa,
        re.DOTALL,
    )
    if not abort_window:
        fails.append("AC5: 3105 abort window (fetch_add then abort) missing")

    must("clear_single_mark_residual", "AC3", soa)
    must("ac3201_1_production_default_on", "AC1 test", t)
    must("ac3201_2_soft_env0", "AC2 test", t)
    must("ac3201_3_batch_clears", "AC3 test", t)
    must("ac3201_4_source_and_linter", "AC5 test", t)
    must("check_ir_dirty_batch_only_production_default_3201", "AC5 build.py", build)
    if "query:ir-dirty-batch-only" in soa:
        fails.append("AC6: new public query key")
    if "g_3201_" in soa:
        fails.append("AC6: invented g_3201_* counter")

    if (ROOT / "tests" / "compiler" / "test_issue_3201.cpp").is_file():
        fails.append("AC6: test_issue_3201.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3201-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3201 production IR dirty batch-only default-on — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
