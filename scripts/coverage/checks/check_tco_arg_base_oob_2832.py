#!/usr/bin/env python3
"""Issue #2832: TCOPass refuses non-zero arg_base when slots are OOB.

Contract (one row per AC):
  AC1 arg_base path bounds-checks vs local_count; Issue #2832; oob metric
  AC2 no residual 'don't re-validate' claim without guard
  AC3 test suite present (OOB refused + in-bounds TCO)
  AC4 linter wired; schema-2832; no docs/design/2832-*; no test_issue_2832.cpp

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
    test = _read("tests/compiler/test_tco_arg_base_oob.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 — run_on_block non-zero arg_base path (not inter-block continue).
    must("Issue #2832", "AC1", impls)
    must("tco_arg_base_oob_skipped", "AC1", impls)
    pos = impls.find("if (arg_base != 0) {")
    if pos < 0:
        pos = impls.find("Issue #2832 contract")
    body = impls[pos : pos + 1200] if pos >= 0 else ""
    must("local_count", "AC1", body)
    must("tco_arg_base_oob_skipped", "AC1", body)
    must("return;", "AC1", body)

    # AC2: residual unsafe claim
    if "we don't re-validate" in body or "we don\\'t re-validate" in body:
        fails.append("AC2: residual 'don't re-validate' in non-zero arg_base path")
    if "assumed to be well-formed, so we don't re-validate" in impls:
        # Allow only if not near the transform loop without a guard after.
        near = impls.find("assumed to be well-formed, so we don't re-validate")
        if near >= 0 and "Issue #2832" not in impls[max(0, near - 200) : near + 400]:
            fails.append("AC2: residual well-formed assumption without #2832 guard")

    # AC3
    must("ac2832", "AC3", test)
    must("2832", "AC3", test)
    must("arg_base", "AC3", test)
    must("local_count", "AC3", test)
    must("tco_arg_base_oob_skipped", "AC3", test)
    must("TCOPass", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_tco_arg_base_oob.cpp").is_file():
        fails.append("AC3: missing test_tco_arg_base_oob.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2832.cpp").is_file():
        fails.append("AC3: test_issue_2832.cpp present (forbidden per #81967)")
    must("test_tco_arg_base_oob", "AC3", cmake)

    # AC4
    must("check_tco_arg_base_oob_2832", "AC4", build)
    must("schema-2832", "AC4", obs)
    must("tco-arg-base-oob-skipped-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2832-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2832 TCOPass arg_base OOB guard")
    return 0


if __name__ == "__main__":
    sys.exit(main())
