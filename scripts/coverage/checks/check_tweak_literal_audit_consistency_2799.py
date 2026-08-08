#!/usr/bin/env python3
"""Issue #2799: tweak-literal Guard/batch abort marks mutation_log RolledBack.

Same torn-audit class as #2793 replace-value. Status logged Committed at
write; rollback_since + rollback_record_for_boundary_abort forces RolledBack.

Contract (one row per AC):
  AC1 public + lockless cite #2799; MutationSoAField::IntVal
  AC2 documents RolledBack / #2793 boundary abort path
  AC3 tests/compiler/test_tweak_literal_audit_consistency.cpp + no test_issue_2799.cpp
  AC4 this linter wired; no docs/design/2799-*

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
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    test = _read("tests/compiler/test_tweak_literal_audit_consistency.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Prefer #2799 comment just above the registration body.
    ppos = mut.find("Issue #2799")
    if ppos < 0:
        ppos = mut.find('add_mutate(\n        "mutate:tweak-literal"')
    if ppos < 0:
        ppos = mut.rfind("mutate:tweak-literal")  # last hit near registration
    pwin = mut[max(0, ppos - 100) : ppos + 4000] if ppos >= 0 else ""

    lpos = flat.find("eval_flat_apply_mutate_tweak_literal")
    lwin = flat[lpos : lpos + 2500] if lpos >= 0 else ""

    # AC1
    must("Issue #2799", "AC1", pwin)
    must("MutationSoAField::IntVal", "AC1", pwin)
    must("Issue #2799", "AC1", lwin)
    must("MutationSoAField::IntVal", "AC1", lwin)

    # AC2
    if "RolledBack" not in lwin and "2793" not in lwin and "rollback_record" not in lwin:
        fails.append("AC2: lockless must document RolledBack / #2793 abort path")
    must("2793", "AC2", pwin)

    # AC3
    must("ac2799", "AC3", test)
    must("2799", "AC3", test)
    must("tweak-literal", "AC3", test)
    must("RolledBack", "AC3", test)
    must("atomic-batch", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_tweak_literal_audit_consistency.cpp").is_file():
        fails.append("AC3: missing test_tweak_literal_audit_consistency.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2799.cpp").is_file():
        fails.append("AC3: test_issue_2799.cpp present (forbidden per #81967)")
    must("test_tweak_literal_audit_consistency", "AC3", cmake)

    # AC4
    must("check_tweak_literal_audit_consistency_2799", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2799-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2799 tweak-literal audit consistency — RolledBack on Guard/batch abort + IntVal")
    return 0


if __name__ == "__main__":
    sys.exit(main())
