#!/usr/bin/env python3
"""
Linter for #2316 — extend lock-order audit to mailbox, hot-update, and
compact_env_frames paths. Closes the deadlock prevention maturity gap:
existing lock_order_audit surface documents Mutate/Workspace/EnvFrames/DepGraph
order (#1388), but multi-fiber production paths now cross mailbox,
hot-update registry, and compact_env_frames_lock_ without a single
documented/audited global order. After #2310-#2315 correctness fixes,
deadlocks remained "convention", not detectable canaries.

Verifies the implementation is wired correctly:
  - src/compiler/lock_order_audit.h has extended Level enum (Mailbox=0,
    Mutate=1, HotUpdate=2, Workspace=3, EnvFrames=4, CompactEnv=5,
    DepGraph=6, kCount=7)
  - Forbidden inversions documented in header comment (decision table)
  - Canonical acquire order: Mailbox → Mutate → HotUpdate → Workspace →
    EnvFrames → CompactEnv → DepGraph
  - Runtime canary AURA_LOCK_ORDER_CANARY=1 + abort with file:line
  - g_lock_order_violation_total counter (canary only)
  - Wire sites: multi_fiber_mailbox.h (Mailbox), evaluator.ixx (Workspace),
    hot_update_registry.cpp (HotUpdate)
  - tests/compiler/test_lock_order_audit_2316.cpp cites Issue #2316

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_lock_order_audit_coverage.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def must_contain(file: Path, needle: str, label: str) -> bool:
    if not file.exists():
        print(f"FAIL {label}: file {file} does not exist")
        return False
    content = read(file)
    if needle in content:
        print(f"OK  {label}")
        return True
    print(f"FAIL {label}: '{needle}' not found in {file}")
    return False


def main() -> int:
    checks = [
        # lock_order_audit.h: extended Level enum
        (ROOT / "src/compiler/lock_order_audit.h", "Mailbox = 0", "lock_order_audit.h: Level::Mailbox = 0"),
        (ROOT / "src/compiler/lock_order_audit.h", "Mutate = 1", "lock_order_audit.h: Level::Mutate = 1"),
        (
            ROOT / "src/compiler/lock_order_audit.h",
            "HotUpdate = 2",
            "lock_order_audit.h: Level::HotUpdate = 2 (#2316 extension)",
        ),
        (ROOT / "src/compiler/lock_order_audit.h", "Workspace = 3", "lock_order_audit.h: Level::Workspace = 3"),
        (ROOT / "src/compiler/lock_order_audit.h", "EnvFrames = 4", "lock_order_audit.h: Level::EnvFrames = 4"),
        (
            ROOT / "src/compiler/lock_order_audit.h",
            "CompactEnv = 5",
            "lock_order_audit.h: Level::CompactEnv = 5 (#2316 extension)",
        ),
        (ROOT / "src/compiler/lock_order_audit.h", "DepGraph = 6", "lock_order_audit.h: Level::DepGraph = 6"),
        (ROOT / "src/compiler/lock_order_audit.h", "DepGraph = 6", "lock_order_audit.h: DepGraph = 6 (#2316 stable)"),
        (ROOT / "src/compiler/lock_order_audit.h", "kCount =", "lock_order_audit.h: kCount present (#2354 may extend)"),
        # Forbidden inversions header comment
        (
            ROOT / "src/compiler/lock_order_audit.h",
            "Forbidden inversions",
            "lock_order_audit.h: forbidden inversions header comment",
        ),
        (
            ROOT / "src/compiler/lock_order_audit.h",
            "Mailbox → Mutate → HotUpdate → Workspace → EnvFrames → CompactEnv → DepGraph",
            "lock_order_audit.h: canonical acquire order documented",
        ),
        # Runtime canary mechanism
        (
            ROOT / "src/compiler/lock_order_audit.h",
            "AURA_LOCK_ORDER_CANARY",
            "lock_order_audit.h: AURA_LOCK_ORDER_CANARY env guard",
        ),
        (
            ROOT / "src/compiler/lock_order_audit.h",
            "lock_order_canary_enabled",
            "lock_order_audit.h: lock_order_canary_enabled() helper",
        ),
        (
            ROOT / "src/compiler/lock_order_audit.h",
            "g_lock_order_canary_enabled",
            "lock_order_audit.h: g_lock_order_canary_enabled atomic",
        ),
        (
            ROOT / "src/compiler/lock_order_audit.h",
            "std::abort()",
            "lock_order_audit.h: abort on inversion under canary",
        ),
        (
            ROOT / "src/compiler/lock_order_audit.h",
            "Production default OFF",
            "lock_order_audit.h: production default OFF documented",
        ),
        # Counter g_lock_order_violation_total
        (
            ROOT / "src/compiler/lock_order_audit.h",
            "g_lock_order_violation_total",
            "lock_order_audit.h: g_lock_order_violation_total counter (canary only)",
        ),
        (ROOT / "src/compiler/lock_order_audit.h", "Issue #2316", "lock_order_audit.h: cites Issue #2316"),
        # Wire sites: mailbox
        (
            ROOT / "src/serve/multi_fiber_mailbox.h",
            '#include "compiler/lock_order_audit.h"',
            "multi_fiber_mailbox.h: includes lock_order_audit.h",
        ),
        (
            ROOT / "src/serve/multi_fiber_mailbox.h",
            "lock_order::Level::Mailbox",
            "multi_fiber_mailbox.h: wires Level::Mailbox",
        ),
        # Wire sites: workspace
        (ROOT / "src/compiler/evaluator.ixx", "lock_order::Level::Workspace", "evaluator.ixx: wires Level::Workspace"),
        # Wire sites: hot-update
        (
            ROOT / "src/compiler/hot_update_registry.cpp",
            '#include "compiler/lock_order_audit.h"',
            "hot_update_registry.cpp: includes lock_order_audit.h",
        ),
        (
            ROOT / "src/compiler/hot_update_registry.cpp",
            "lock_order::Level::HotUpdate",
            "hot_update_registry.cpp: wires Level::HotUpdate",
        ),
        # Test file
        (ROOT / "tests/compiler/test_lock_order_audit_2316.cpp", "Issue #2316", "test file cites 2316"),
        (ROOT / "tests/compiler/test_lock_order_audit_2316.cpp", "ac2316_rank_table", "test file has AC1 function"),
        (
            ROOT / "tests/compiler/test_lock_order_audit_2316.cpp",
            "ac2316_canary_mechanism",
            "test file has AC2 function",
        ),
        # Linter self-reference (sanity)
        (ROOT / "scripts/check_lock_order_audit_coverage.py", "extend lock-order audit", "linter self-reference"),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2316 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
