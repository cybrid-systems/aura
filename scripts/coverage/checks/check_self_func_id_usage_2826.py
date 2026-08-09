#!/usr/bin/env python3
"""Issue #2826: self_func_id footgun — never test id != 0 alone.

Contract (one row per AC):
  AC1 LoweringState helpers is_self / matches_self_name; #2826 cites
  AC2 call sites use matches_self_name or self_func_active (not bare id)
  AC3 no bare self_func_id != 0 / == 0 / > 0 in src/compiler (except helpers)
  AC4 this linter wired; test present; no docs/design/2826-*; no test_issue_2826.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Bare comparisons that reintroduce #2292 (id 0 is a valid func id).
BARE_ID_CMP = re.compile(
    r"self_func_id\s*(?:!=|==|>|<|>=|<=)\s*0\b"
    r"|"
    r"\b0\s*(?:!=|==|>|<|>=|<=)\s*self_func_id\b"
)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _iter_compiler_sources() -> list[Path]:
    src = ROOT / "src" / "compiler"
    out: list[Path] = []
    for p in src.rglob("*"):
        if p.suffix in {".ixx", ".cpp", ".h", ".hh", ".hpp"}:
            out.append(p)
    return out


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    low = _read("src/compiler/lowering.ixx")
    impl = _read("src/compiler/lowering_impl.cpp")
    test = _read("tests/compiler/test_self_func_active_invariant.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2826", "AC1", low)
    must("is_self(", "AC1", low)
    must("matches_self_name", "AC1", low)
    must("self_func_active", "AC1", low)
    must("static_assert", "AC1", low)

    # AC2: Variable self paths use helper
    must("matches_self_name", "AC2", impl)
    must("Issue #2826", "AC2", impl)

    # AC3: bare id comparisons forbidden in src/compiler (helpers/docs OK)
    for p in _iter_compiler_sources():
        text = p.read_text(encoding="utf-8", errors="replace")
        rel = str(p.relative_to(ROOT))
        for i, line in enumerate(text.splitlines(), 1):
            stripped = line.strip()
            # Allow comments / string docs that mention the footgun pattern.
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            if "never test id" in line or "self_func_id != 0 alone" in line:
                continue
            if "static_assert" in line:
                continue
            if BARE_ID_CMP.search(line):
                fails.append(f"AC3: bare self_func_id cmp in {rel}:{i}: {stripped[:80]}")

    # AC4
    must("ac2826", "AC4", test)
    must("2826", "AC4", test)
    must("matches_self_name", "AC4", test)
    must("is_self", "AC4", test)
    if not (ROOT / "tests" / "compiler" / "test_self_func_active_invariant.cpp").is_file():
        fails.append("AC4: missing test_self_func_active_invariant.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2826.cpp").is_file():
        fails.append("AC4: test_issue_2826.cpp present (forbidden per #81967)")
    must("test_self_func_active_invariant", "AC4", cmake)
    must("check_self_func_id_usage_2826", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2826-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2826 self_func_id usage — helpers + no bare id!=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
