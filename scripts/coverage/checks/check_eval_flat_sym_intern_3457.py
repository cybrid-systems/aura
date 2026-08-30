#!/usr/bin/env python3
"""Issue #3457: eval_flat intern keys by SymId, not hashed string_view.

#3401 closed every-LiteralString std::string. Residual: intern maps
hashed pool string_view. Hit is now a dense SymId probe. Miss still
allocates the heap payload once.

Contract:
  AC1 second eval of same LiteralString sym_id does not push_back
  AC2 :foo second hit does not keyword_table_.push_back
  AC3 Env::lookup stays string_view; no function-scope try (#3401)
  AC4 #2616 classify ban preserved on eval_flat TU
  AC5 no docs/design/3457-*; no test_issue_3457.cpp; no new query key

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


def _strip(src: str) -> str:
    out = re.sub(r"//[^\n]*", "", src)
    out = re.sub(r"/\*.*?\*/", "", out, flags=re.S)
    return out


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    env = _read("src/compiler/evaluator_env.cpp")
    t3401 = _read("tests/core/test_arena_required_cover_no_value_only.cpp")
    tir = _read("tests/compiler/test_ir.cpp")
    l3401 = _read("scripts/check_eval_flat_hot_path_3401.py")
    build = _read("build.py")
    stripped = _strip(flat)

    must("kEvalFlatSymInternIssue = 3457", "AC1 stamp", ev)
    must("string_intern_by_sym_", "AC1 member", ev)
    must("keyword_intern_by_sym_", "AC2 member", ev)
    must("string_intern_by_sym_.get(v.sym_id)", "AC1 get", flat)
    must("string_intern_by_sym_.set(v.sym_id", "AC1 set", flat)
    must("keyword_intern_by_sym_.get(v.sym_id)", "AC2 get", flat)
    must("keyword_intern_by_sym_.set(v.sym_id", "AC2 set", flat)
    must("Issue #3457", "AC1 cite", flat)

    lit = flat.find("// Issue #3401: happy-path string intern")
    lwin = flat[lit : lit + 1600] if lit >= 0 else ""
    if "string_intern_.find" in lwin:
        fails.append("AC1: LiteralString arm still hashes string_intern_.find")
    must("std::string raw(raw_sv)", "AC1 miss still allocates", lwin)
    must("string_heap_.push_back", "AC1 miss still push_back", lwin)

    kw = flat.find("// Issue #3401: keyword O(1) intern")
    kwin = flat[kw : kw + 1600] if kw >= 0 else ""
    if "keyword_intern_.find" in kwin:
        fails.append("AC2: :foo arm still hashes keyword_intern_.find")
    must("keyword_table_.push_back", "AC2 miss still push_back", kwin)

    must("Env::lookup(std::string_view n)", "AC3 lookup", env)
    if "eval_env.lookup(std::string(name))" in flat:
        fails.append("AC3: lookup still constructs std::string")
    must("Issue #3401: production (NDEBUG) builds skip the function-scope", "AC3 try wrap", flat)
    if re.search(r"\bclassify_eval_value_tag\s*\(", stripped):
        fails.append("AC4: classify_eval_value_tag reintroduced (#2616)")

    must("string_intern_by_sym_.get(v.sym_id)", "AC1 extend 3401 test", t3401)
    must("test_literal_string_sym_intern", "AC1 eval_flat fixture", tir)
    must("check_eval_flat_hot_path_3401", "AC1 reuse 3401 linter", l3401)
    must("check_eval_flat_sym_intern_3457", "AC5 build.py", build)
    if "schema-3457" in ev or "schema-3457" in flat:
        fails.append("AC5: new schema-3457 query key")
    if (ROOT / "tests" / "core" / "test_issue_3457.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3457.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3457.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3457.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3457-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3457 eval_flat intern by SymId — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
