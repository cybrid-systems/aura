#!/usr/bin/env python3
"""Issue #2831: TCOPass sweeps unreachable dead blocks after TCO.

Contract (one row per AC):
  AC1 sweep_dead_blocks called from TCOPass::run; Issue #2831
  AC2 tco_dead_block_total metric
  AC3 test suite present (inter-block reclaim + repeated runs)
  AC4 linter wired; schema-2831; no docs/design/2831-*; no test_issue_2831.cpp

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

    impls = _read("src/compiler/pass_impls.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_tco_dead_block_accumulation.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 — TCOPass region
    must("Issue #2831", "AC1", impls)
    must("sweep_dead_blocks", "AC1", impls)
    must("sweep_dead_blocks(func)", "AC1", impls)
    # Reachability must consider Jump + Branch targets.
    pos = impls.find("void sweep_dead_blocks")
    body = impls[pos : pos + 2000] if pos >= 0 else ""
    must("IROpcode::Jump", "AC1", body)
    must("IROpcode::Branch", "AC1", body)
    must("entry_block", "AC1", body)

    # AC2
    must("tco_dead_block_total", "AC2", impls)
    must("tco_dead_block_count_", "AC2", impls)

    # AC3
    must("ac2831", "AC3", test)
    must("2831", "AC3", test)
    must("TCOPass", "AC3", test)
    must("blocks.size()", "AC3", test)
    must("sweep_dead_blocks", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_tco_dead_block_accumulation.cpp").is_file():
        fails.append("AC3: missing test_tco_dead_block_accumulation.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2831.cpp").is_file():
        fails.append("AC3: test_issue_2831.cpp present (forbidden per #81967)")
    must("test_tco_dead_block_accumulation", "AC3", cmake)

    # AC4
    must("check_tco_dead_block_accumulation_2831", "AC4", build)
    must("schema-2831", "AC4", obs)
    must("tco-dead-block-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2831-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2831 TCOPass dead-block sweep after inter-block TCO")
    return 0


if __name__ == "__main__":
    sys.exit(main())
