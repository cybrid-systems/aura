#!/usr/bin/env python3
"""Issue #2820: seal last SoA block after alloc_block dual-emit.

Contract (one row per AC):
  AC1 finalize_soa_module / finalize_last_blocks / #2820 cites
  AC2 set_cur_function seals previous last block
  AC3 test suite + unsealed metric
  AC4 linter wired; schema-2820; no docs/design/2820-*; no test_issue_2820.cpp

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

    low = _read("src/compiler/lowering.ixx")
    impl = _read("src/compiler/lowering_impl.cpp")
    soa = _read("src/compiler/ir_soa.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_alloc_block_seal_last.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2820", "AC1", low)
    must("finalize_soa_module", "AC1", low)
    must("seal_soa_function_last_block", "AC1", low)
    must("finalize_last_blocks", "AC1", soa)
    must("g_lowering_alloc_block_unsealed_total_atomic", "AC1", soa)
    must("finalize_soa_module", "AC1", impl)
    must("Issue #2820", "AC1", impl)

    # AC2
    scf = low.find("void set_cur_function")
    scf_body = low[scf : scf + 900] if scf >= 0 else ""
    must("seal_soa_function_last_block", "AC2", scf_body)
    must("alloc_block", "AC2", low)

    # AC3
    must("ac2820", "AC3", test)
    must("2820", "AC3", test)
    must("finalize_last_blocks", "AC3", test)
    must("finalize_soa_module", "AC3", test)
    must("g_lowering_alloc_block_unsealed_total_atomic", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_alloc_block_seal_last.cpp").is_file():
        fails.append("AC3: missing test_alloc_block_seal_last.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2820.cpp").is_file():
        fails.append("AC3: test_issue_2820.cpp present (forbidden per #81967)")
    must("test_alloc_block_seal_last", "AC3", cmake)

    # AC4
    must("check_alloc_block_seal_last_2820", "AC4", build)
    must("schema-2820", "AC4", obs)
    must("lowering-alloc-block-unsealed-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2820-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2820 alloc_block seal last — SoA tail instructions visible")
    return 0


if __name__ == "__main__":
    sys.exit(main())
