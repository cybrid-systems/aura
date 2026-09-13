#!/usr/bin/env python3
"""Issue #3703 source-cite gate: quote intern-by-sym + alloc lock scope.

ast_to_data's LiteralString case pushed a fresh string_heap_ entry for
every quote of the same sym_id (eval_flat LiteralString interns by sym
via string_intern_by_sym_; the quote path did not), and the Call/Begin
cases held alloc_storage_lock_ — the evaluator-wide recursive mutex —
across the whole ast_to_data recursion, serializing multi-fiber
quote/eval of lists on that lock.

ACs:
  AC1  ast_to_data LiteralString interns by sym_id (get hit path, one
       heap push, set) with the #3703 cite.
  AC2  Call + Begin recurse WITHOUT alloc_storage_lock_ (items collected
       unlocked; the lock covers only the pairs_.push_back slots).
  AC3  eval_flat LiteralString intern-by-sym unchanged (#3401/#3457).
  AC4  tests cite #3703; no test_issue_3703.cpp; no docs/design/3703-*;
       no new query key.
  AC5  build.py wires this linter.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ME = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
TST = ROOT / "tests" / "compiler" / "test_primcall_str_intern.cpp"


def main() -> int:
    me = ME.read_text() if ME.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    good = (
        "Issue #3703: intern by sym_id" in me
        and "if (const auto* e = string_intern_by_sym_.get(v.sym_id))" in me
        and "string_intern_by_sym_.set(v.sym_id, val);" in me
    )
    report("AC1", good, "quote LiteralString interns by sym_id")

    good = (
        "Issue #3703: recurse WITHOUT alloc_storage_lock_" in me
        and "Issue #3703: same unlocked-recursion shape as Call." in me
    )
    report("AC2", good, "Call/Begin recurse unlocked; lock covers push slots")

    good = "Issue #3457: lookup by v.sym_id (dense), not hashed" in me
    report("AC3", good, "eval_flat intern-by-sym unchanged")

    good = (
        "#3703" in tst
        and not (ROOT / "tests" / "compiler" / "test_issue_3703.cpp").exists()
        and not (ROOT / "docs" / "design" / "3703-quote-intern-lock.md").exists()
    )
    report("AC4", good, "tests cite #3703; no new artifacts")

    good = "check_quote_intern_lock_3703.py" in build
    report("AC5", good, "build.py wires this linter")

    print("Issue #3703 quote intern / lock scope linter: " + ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
