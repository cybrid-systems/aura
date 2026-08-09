#!/usr/bin/env python3
"""Issue #2819: capture_audit_event_forced lock-free trail publish.

Contract (one row per AC):
  AC1 capture_audit_event_forced has no lock_guard; #2819; lockfree metric
  AC2 metrics: audit_trail_lockfree_total + mutex_wait_us + wired
  AC3 test suite + concurrent stress
  AC4 linter wired; schema-2819; no docs/design/2819-*; no test_issue_2819.cpp

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    h = _read("src/compiler/typed_mutation_audit.h")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_audit_trail_lockfree.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    def_pos = h.find("inline void capture_audit_event_forced")
    next_pos = h.find("inline void capture_audit_event(", def_pos + 1 if def_pos >= 0 else 0)
    if def_pos < 0:
        fails.append("AC1: capture_audit_event_forced definition missing")
        body = ""
    else:
        end = next_pos if next_pos > def_pos else def_pos + 2500
        body = h[def_pos:end]

    # AC1
    must("Issue #2819", "AC1", body)
    must("audit_trail_lockfree_total", "AC1", body)
    must("g_trail().ring", "AC1", body)
    must_not("lock_guard", "AC1", body)
    must_not("std::lock_guard lock(", "AC1", body)
    must_not("lock(g_trail()", "AC1", body)

    # AC2
    must("audit_trail_lockfree_total", "AC2", h)
    must("audit_trail_mutex_wait_us_total", "AC2", h)
    must("audit_trail_lockfree_wired", "AC2", h)
    must("schema-2819", "AC2", obs)
    must("audit-trail-lockfree-total", "AC2", obs)
    must("audit-trail-mutex-wait-us-total", "AC2", obs)

    # AC3
    must("ac2819", "AC3", test)
    must("2819", "AC3", test)
    must("capture_audit_event_forced", "AC3", test)
    must("std::thread", "AC3", test)
    must("audit_trail_lockfree_total", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_audit_trail_lockfree.cpp").is_file():
        fails.append("AC3: missing test_audit_trail_lockfree.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2819.cpp").is_file():
        fails.append("AC3: test_issue_2819.cpp present (forbidden per #81967)")
    must("test_audit_trail_lockfree", "AC3", cmake)

    # AC4
    must("check_audit_trail_lockfree_2819", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2819-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2819 audit trail lock-free publish — no hot-path mutex")
    return 0


if __name__ == "__main__":
    sys.exit(main())
