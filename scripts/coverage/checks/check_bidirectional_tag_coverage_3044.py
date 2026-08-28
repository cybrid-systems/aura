#!/usr/bin/env python3
"""Issue #3044: exhaustive bidirectional synthesize/check NodeTag coverage.

Contract (one row per AC):
  AC1  Production/strict uncovered tag → TypeError / hard reject before commit
  AC2  Soft: Warning + counter only (no unit-test behavior change)
  AC3  Covered path zero extra cost (gate only in synthesize_flat default)
  AC4  schema-3044 + extend test_bidirectional_match_check
  AC5  source cites type_checker_impl dispatch + NodeTag; no docs/design/
  AC6  this linter wired in build.py; no test_issue_3044.cpp

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


def _enum_tags(ast: str) -> list[str]:
    m = re.search(r"enum class NodeTag\s*:\s*std::uint32_t\s*\{(.*?)\};", ast, re.S)
    if not m:
        return []
    names: list[str] = []
    for line in m.group(1).splitlines():
        line = line.split("//", 1)[0].strip()
        if not line or line.startswith("/*"):
            continue
        mm = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*=", line)
        if mm:
            names.append(mm.group(1))
    return names


def _switch_cases(impl: str) -> set[str]:
    # synthesize_flat switch (v.tag)
    start = impl.find("TypeId InferenceEngine::synthesize_flat")
    if start < 0:
        return set()
    body = impl[start : start + 8000]
    return set(re.findall(r"case\s+Tag::([A-Za-z_][A-Za-z0-9_]*)", body))


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ast = _read("src/core/ast.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    hdr = _read("src/compiler/type_checker.ixx")
    ev = _read("src/compiler/evaluator_typecheck.cpp")
    prim = _read("src/compiler/evaluator_primitives_compile.cpp")
    test = _read("tests/compiler/test_bidirectional_match_check.cpp")
    build = _read("build.py")

    tags = _enum_tags(ast)
    cases = _switch_cases(impl)
    if not tags:
        fails.append("AC5: NodeTag enum not parsed")
    for name in tags:
        if f"T::{name}" not in hdr and f"case T::{name}" not in hdr:
            fails.append(f"AC5: coverage table missing {name}")
        if name not in cases:
            fails.append(f"AC5: synthesize_flat switch missing case {name}")

    # ── AC1 ──
    must("note_uncovered_bidirectional_tag", "AC1", impl)
    must("ErrorKind::TypeError", "AC1", impl)
    must("uncovered bidirectional tag", "AC1 mutate", ev)
    must("production fail-closed", "AC1 mutate", ev)
    must("3044 AC1", "AC1 test", test)

    # ── AC2 ──
    must("ErrorKind::Warning", "AC2", impl)
    must("3044 AC2", "AC2 test", test)

    # ── AC3 ──
    must("is_bidirectional_tag_covered", "AC3", hdr)
    must("zero extra cost", "AC3", impl)
    must("3044 AC3", "AC3 test", test)
    must("bidirectional_coverage_table_complete", "AC3", hdr)

    # ── AC4 ──
    must("schema-3044", "AC4 query", prim)
    must("uncovered-tag-total", "AC4 query", prim)
    must("uncovered-tag-wired", "AC4 query", prim)
    must("3044 AC4", "AC4 test", test)
    must("bidirectional_uncovered_tag_total", "AC4 metrics", _read("src/compiler/observability_metrics.h"))

    # ── AC5 ──
    must("enum class NodeTag", "AC5 ast", ast)
    must("Issue #3044", "AC5 impl", impl)
    must("kBidirectionalUncoveredTagIssue = 3044", "AC5 hdr", hdr)
    must("kBidirectionalUncoveredNoDynamicIssue = 3330", "AC5 3330 stamp", hdr)
    must("Issue #3330", "AC5 3330 impl", impl)
    must("never Dynamic", "AC5 3330 never Dynamic", impl)
    must("return reg_.void_type()", "AC5 3330 void path", impl)
    must("Soft only", "AC5 3330 Soft Dynamic", impl)
    must("3330 AC1", "AC5 3330 test", test)

    # ── AC6 ──
    must("check_bidirectional_tag_coverage_3044", "AC6", build)
    must("check_bidirectional_uncovered_no_dynamic_3330", "AC6 3330", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3044.cpp").is_file():
        fails.append("AC6: test_issue_3044.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3044-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3044 exhaustive bidirectional tag coverage — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
