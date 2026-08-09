#!/usr/bin/env python3
"""Issue #2869: nested fiber:join-in-worker must not hang on thread backend.

CLI thread-fallback serializes fiber bodies with s_cli_thread_fiber_body_mtx
(#2738). Holding that mutex while fiber:join waits for a child that needs
the same mutex deadlocks. Fix: TLS pointer to the body unique_lock +
CliBodyLockJoinGuard unlocks around join wait, relocks after.

Contract:
  AC1 TLS body-lock pointer set in complete_fiber (thread backend)
  AC2 CliBodyLockJoinGuard unlocks owns_lock around wait
  AC3 stdin + degrade join paths use the guard
  AC4 suite tests/suite/nested_fiber_join_2869.aura
  AC5 linter wired in build.py; no docs/design/*

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

    msg = _read("src/compiler/evaluator_primitives_messaging.cpp")
    suite = _read("tests/suite/nested_fiber_join_2869.aura")
    build = _read("build.py")

    # AC1 — TLS body lock for nested join unlock
    must("#2869", "AC1", msg)
    must("s_tls_cli_body_lock", "AC1", msg)
    must("s_cli_thread_fiber_body_mtx", "AC1", msg)
    must("TlsBodyLockScope", "AC1", msg)

    # AC2 — join guard unlocks / relocks
    must("CliBodyLockJoinGuard", "AC2", msg)
    must("owns_lock", "AC2", msg)
    must("unlock()", "AC2", msg)

    # AC3 — both cv wait paths use the guard
    must("CliBodyLockJoinGuard body_join_guard", "AC3", msg)
    if msg.count("CliBodyLockJoinGuard body_join_guard") < 2:
        fails.append("AC3: expect body_join_guard on stdin + degrade join paths")

    # AC4 — suite regression
    must("2869", "AC4", suite)
    must("nested join=7", "AC4", suite)
    must("outer sum=3", "AC4", suite)
    must("OK-2869", "AC4", suite)

    # AC5 — wire + no docs
    must("check_nested_fiber_join_2869", "AC5", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2869.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2869.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2869-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2869 nested fiber:join-in-worker — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
