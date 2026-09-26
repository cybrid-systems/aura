#!/usr/bin/env python3
"""Issue #4121: query:typed-mutation-audit-trail planned_keys headroom.

planned_keys must be >= live insert_kv count + 8. The handler uses
insert_kv_checked and query_hash_finish so a probe miss publishes
hash-overflow. No new query key.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HEADROOM = 8
INSERT_RE = re.compile(r'insert_kv\(\s*"')


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []
    src = _read("src/compiler/evaluator_primitives_mutate.cpp")
    i = src.find('"query:typed-mutation-audit-trail"')
    j = src.find("return query_hash_finish", i if i >= 0 else 0)
    if i < 0 or j < 0:
        fails.append("AC1: trail handler missing")
        block = ""
    else:
        block = src[i:j]
    live = len(INSERT_RE.findall(block))
    m = re.search(r"kTypedMutationAuditTrailPlannedKeys\s*=\s*(\d+)", block)
    planned = int(m.group(1)) if m else 0
    if planned < live + HEADROOM:
        fails.append(f"AC1: planned {planned} < live {live} + {HEADROOM}")
    if "insert_kv_checked" not in block:
        fails.append("AC3: handler does not use insert_kv_checked")
    if "query_hash_finish" not in src[j : j + 80] if j >= 0 else True:
        fails.append("AC3: handler does not finish through query_hash_finish")
    if "hash-overflow" not in _read("src/compiler/evaluator.ixx"):
        fails.append("AC2: query_hash_finish does not publish hash-overflow")
    test = _read("tests/compiler/test_composite_nested_txn_invariant_audit.cpp")
    if "4121: force_cap hash-overflow" not in test:
        fails.append("AC5: force_cap test missing")
    if "check_typed_mutation_audit_trail_headroom_4121" not in _read("build.py"):
        fails.append("AC4: build.py does not wire the linter")
    if (ROOT / "tests" / "compiler" / "test_issue_4121.cpp").exists():
        fails.append("AC5: test_issue_4121.cpp must not exist")
    if any((ROOT / "docs" / "design").glob("4121-*")):
        fails.append("AC5: docs/design/4121-* must not exist")
    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(f"OK: Issue #4121 typed-mutation-audit-trail headroom (live={live} planned={planned})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
