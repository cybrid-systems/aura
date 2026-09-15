#!/usr/bin/env python3
"""Issue #3829: children_columnar SafePCVSpan across-Guard fingerprint.

Pre-#3829 FlatAST::children_columnar returned a 1-arg SafePCVSpan (span
only — no node_id/gen/wrap/node_gen fingerprint). pin_query_children uses
columnar, so Production has_fingerprint() gates were no-ops on dense
spans. Fingerprinted PCV children_safe_view already detected across-Guard
stale (#3167/#3328). Soft pcv_span_for_agent_export remains identity.

Contract:
  AC1 Capture columnar → mutate → Production is_stale / re-pin live
  AC2 Soft frozen view unchanged (export identity)
  AC3 Fingerprinted PCV path unchanged; suite / stamp / build wiring;
      no invent test_issue_*.cpp; no docs/design/

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

    hh = _read("src/core/persistent_child_vector.hh")
    ast = _read("src/core/ast.ixx")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    t = _read("tests/core/test_pcv_exclusive_with_set.cpp")
    build = _read("build.py")

    must("kPcvDenseColumnarFingerprintIssue = 3829", "AC1 stamp", hh)
    must("Issue #3829: dense columnar span with across-Guard fingerprint", "AC1 dense ctor", hh)
    must("Issue #3829", "AC1 ast cite", ast)
    must("children_columnar", "AC1 columnar", ast)
    # Dense path must stamp fingerprint fields (not 1-arg-only return).
    col_anchor = ast.find("[[nodiscard]] SafePCVSpan<NodeId> children_columnar(NodeId id) const")
    if col_anchor < 0:
        fails.append("AC1: children_columnar definition missing")
        col_body = ""
    else:
        col_body = ast[col_anchor : col_anchor + 2200]
    must("has_fingerprint()", "AC1 fingerprint cite", col_body)
    must("node_id+generation_+wrap_epoch_", "AC1 capture fields cite", col_body)
    if "generation_" not in col_body or "wrap_epoch_" not in col_body or "node_gen_" not in col_body:
        fails.append("AC1: children_columnar must capture generation_/wrap_epoch_/node_gen_")
    if "SafePCVSpan<NodeId>(std::span<const NodeId>(child_data_.data() + begin, count));" in col_body:
        fails.append("AC1: children_columnar still uses 1-arg SafePCVSpan (no fingerprint)")
    must("ac3829_1_columnar_production_stale", "AC1 test", t)

    must("if (!production)", "AC2 Soft identity helper", ast)
    must("ac3829_2_soft_frozen_view_unchanged", "AC2 test", t)
    must("pcv_span_for_agent_export", "AC2 export helper", ast)

    must("children_safe_view", "AC3 PCV path", ast)
    must("ac3829_3_pcv_path_unchanged", "AC3 test", t)
    must("Issue #3829", "AC3 pin_query_children cite", qws)
    must("check_dense_columnar_fingerprint_3829", "AC3 build.py", build)
    must("kPcvSpanStaleAcrossGuardIssue = 3167", "AC3 #3167 lineage", hh)
    must("kPcvSpanQueryRefreshIssue = 3328", "AC3 #3328 lineage", hh)

    if (ROOT / "tests" / "core" / "test_issue_3829.cpp").is_file():
        fails.append("AC3: tests/core/test_issue_3829.cpp present (forbidden invent)")
    if (ROOT / "tests" / "issues" / "test_issue_3829.cpp").is_file():
        fails.append("AC3: tests/issues/test_issue_3829.cpp present")
    if (ROOT / "tests" / "compiler" / "test_issue_3829.cpp").is_file():
        fails.append("AC3: tests/compiler/test_issue_3829.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3829-*")):
            fails.append(f"AC3: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print(f"Issue #3829 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3829 dense columnar SafePCVSpan fingerprint — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
