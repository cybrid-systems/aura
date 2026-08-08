#!/usr/bin/env python3
"""Issue #2793: replace-value Guard abort keeps mutation_log audit consistent.

Records are logged Committed before Guard commit. On abort, values reverse
and status must become RolledBack (no torn audit).

Contract (one row per AC):
  AC1 replace-value + rollback_record_for_boundary_abort cite #2793
  AC2 Int uses MutationSoAField::IntVal; status force-mark helper present
  AC3 tests/compiler/test_replace_value_audit_consistency.cpp + no test_issue_2793.cpp
  AC4 this linter wired; no docs/design/2793-*

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    ast = _read("src/core/ast.ixx")
    test = _read("tests/compiler/test_replace_value_audit_consistency.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Prefer the add_mutate registration body (first hit can be a comment).
    pos = mut.find('add_mutate(\n        "mutate:replace-value"')
    if pos < 0:
        pos = mut.find('add_mutate(\n        "mutate:replace-value"')
    if pos < 0:
        pos = mut.find('"mutate:replace-value"')
    if pos < 0:
        fails.append("AC1: mutate:replace-value not found")
        win = ""
    else:
        # Include preceding #2793 comment block + full LiteralInt/Float/Sym body.
        win = mut[max(0, pos - 600) : pos + 4500]

    # AC1
    must("Issue #2793", "AC1", win)
    must("rollback_record_for_boundary_abort", "AC1", ast)
    must("Issue #2793", "AC1", ast)
    must("mutation_log_status_torn_total", "AC1", ast)

    # AC2
    must("MutationSoAField::IntVal", "AC2", win)
    must("MutationStatus::RolledBack", "AC2", ast)
    # Force-mark path when inverse fails
    if "mutation_log_status_torn_total_" not in ast:
        fails.append("AC2: missing mutation_log_status_torn_total_ member")

    # AC3
    must("ac2793", "AC3", test)
    must("2793", "AC3", test)
    must("RolledBack", "AC3", test)
    must("replace-value", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_replace_value_audit_consistency.cpp").is_file():
        fails.append("AC3: missing test_replace_value_audit_consistency.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2793.cpp").is_file():
        fails.append("AC3: test_issue_2793.cpp present (forbidden per #81967)")
    must("test_replace_value_audit_consistency", "AC3", cmake)

    # AC4
    must("check_replace_value_audit_consistency_2793", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2793-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2793 replace-value audit consistency — RolledBack on Guard abort + torn counter")
    return 0


if __name__ == "__main__":
    sys.exit(main())
