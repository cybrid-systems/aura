#!/usr/bin/env python3
"""Issue #3215: stabilize Agent reject reasons for macro-introduced / rest-unmarked.

Residual of #3029: `aura_macro_hygiene_last_limit_reason_string()` only
returned gensym-ceiling / depth-limit / pass-limit. MacroIntroduced
default-deny and rest-param unmarked detection emitted merr("hygiene", …)
or counters without a stable reason enum for replay / self-evo evidence.

Contract (one row per AC):
  AC1  string switch has hygiene-macro-introduced (4) and
       hygiene-rest-unmarked (5); note_hygiene_last_limit_reason helper
  AC2  production reject sites store code 4 (mutate helpers + lockless
       note_*_hygiene_reject) and rest incomplete stores code 5
  AC3  query:macro-hygiene-stats maps codes 4/5 on last-hygiene-limit-reason
       (no new query:* name)
  AC4  Soft/Off: code 4 extra-store gated on sandbox-active /
       production_defaults probe (quiet path never calls helper)
  AC5  hygiene_mutate_closed_loop + rest_param_hygiene assert the strings;
       this linter in build.py; no docs/design / invent file

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

    mx = _read("src/compiler/macro_expansion.cpp")
    ixx = _read("src/compiler/macro_expansion.ixx")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    efl = _read("src/compiler/evaluator_eval_flat.cpp")
    q = _read("src/compiler/evaluator_primitives_query_obs_mid.cpp")
    loop = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    rest = _read("tests/compiler/test_rest_param_hygiene.cpp")
    build = _read("build.py")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    hdr = _read("src/compiler/aura_jit_bridge.h")

    must("hygiene-macro-introduced", "AC1 string", mx)
    must("hygiene-rest-unmarked", "AC1 rest string", mx)
    must("kHygieneLimitReasonMacroIntroduced = 4", "AC1 const 4", ixx)
    must("kHygieneLimitReasonRestUnmarked = 5", "AC1 const 5", ixx)
    must("void note_hygiene_last_limit_reason", "AC1 helper", ixx)
    must("case 4:", "AC1 switch 4", mx)
    must("case 5:", "AC1 switch 5", mx)

    must("kHygieneLimitReasonMacroIntroduced", "AC2 mutate helper", mut)
    must("note_hygiene_last_limit_reason", "AC2 mutate note", mut)
    must("note_hygiene_last_limit_reason(kHygieneLimitReasonMacroIntroduced)", "AC2 lockless", efl)
    must("kHygieneLimitReasonRestUnmarked", "AC2 rest store", mx)
    must("note_hygiene_last_limit_reason(kHygieneLimitReasonRestUnmarked)", "AC2 rest incomplete", mx)

    must("hygiene-limit-reason-macro-introduced", "AC3 query 4", q)
    must("hygiene-limit-reason-rest-unmarked", "AC3 query 5", q)
    must("last-hygiene-limit-reason", "AC3 existing key", q)
    must("schema-3215", "AC3 schema", q)
    if "query:hygiene-agent-reason" in q or "query:macro-hygiene-limit-string" in q:
        fails.append("AC3: new query:* name (reuse last-hygiene-limit-reason)")

    must("note_hygiene_last_limit_reason", "AC4 helper", mx)
    must("!is_macro_introduced", "AC4 quiet-path cite", mx)

    must("hygiene-macro-introduced", "AC5 mutate suite", loop)
    must("ac3215_macro_introduced_reason_string", "AC5 mutate AC", loop)
    must("hygiene-rest-unmarked", "AC5 rest suite", rest)
    must("check_hygiene_agent_reason_strings_3215", "AC5 build.py", build)
    must("aura_note_macro_hygiene_last_limit_reason", "AC5 C ABI", hdr)
    must("aura_note_macro_hygiene_last_limit_reason", "AC5 stub", stub)

    if (ROOT / "tests" / "compiler" / "test_issue_3215.cpp").is_file():
        fails.append("AC5: test_issue_3215.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3215.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3215.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3215-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print(f"Issue #3215 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3215 Agent hygiene reason strings — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
