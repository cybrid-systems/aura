#!/usr/bin/env python3
"""Issue #2825: dual-emit stamps per-instruction SoA source_marker.

Contract (one row per AC):
  AC1 source_markers_ column; add_instruction source_marker param; #2825
  AC2 emit() dual-emit passes last_aos.source_marker / sm
  AC3 metrics stamped + mismatch; to_aos_view copies marker
  AC4 linter wired; schema-2825; no docs/design/2825-*; no test_issue_2825.cpp

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

    soa = _read("src/compiler/ir_soa.ixx")
    low = _read("src/compiler/lowering.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_emit_soa_source_marker_propagation.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("source_markers_", "AC1", soa)
    must("Issue #2825", "AC1", soa)
    must("source_marker = 0", "AC1", soa)
    must("source_marker()", "AC1", soa)
    must("g_lowering_soa_source_marker_stamped_total_atomic", "AC1", soa)
    must("g_lowering_soa_source_marker_mismatch_total_atomic", "AC1", soa)

    # AC2
    must("Issue #2825", "AC2", low)
    must("add_instruction", "AC2", low)
    # emit dual path passes source_marker (sm or last_aos.source_marker)
    emit_pos = low.find("module_v2.add_instruction")
    emit_win = low[emit_pos : emit_pos + 600] if emit_pos >= 0 else ""
    if "source_marker" not in emit_win and ", sm)" not in emit_win and "sm)" not in emit_win:
        # also accept sm variable in surrounding window
        surround = low[max(0, emit_pos - 200) : emit_pos + 600] if emit_pos >= 0 else ""
        if "source_marker" not in surround and "sm" not in surround:
            fails.append("AC2: dual-emit add_instruction missing source_marker/sm")
    must("g_lowering_soa_source_marker_stamped_total_atomic", "AC2", low)

    # AC3
    must("source_marker", "AC3", soa)  # to_aos_view
    must("ac2825", "AC3", test)
    must("2825", "AC3", test)
    must("source_markers_", "AC3", test)
    must("source_marker", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_emit_soa_source_marker_propagation.cpp").is_file():
        fails.append("AC3: missing test_emit_soa_source_marker_propagation.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2825.cpp").is_file():
        fails.append("AC3: test_issue_2825.cpp present (forbidden per #81967)")
    must("test_emit_soa_source_marker_propagation", "AC3", cmake)

    # AC4
    must("check_emit_soa_source_marker_propagation_2825", "AC4", build)
    must("schema-2825", "AC4", obs)
    must("lowering-soa-source-marker-stamped-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2825-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2825 emit SoA source_marker propagation — hygiene parity")
    return 0


if __name__ == "__main__":
    sys.exit(main())
