#!/usr/bin/env python3
"""
Linter for #2312 — fail-closed delivery gate on target MutationBoundary in
MultiFiberMailbox::push / broadcast_fanout. Closes the lock-order inversion
vs workspace_mtx_ + GcDeferReason::MutationHold under multi-agent fanout
(mailbox×mutate was a silent corruption vector in production).

Verifies the implementation is wired correctly:
  - multi_fiber_mailbox.h MultiFiberMailboxStats has new counter
  - multi_fiber_mailbox.h push() gates on target MutationSafetySnapshot when
    msg.to_fiber resolves to an attached fiber
  - multi_fiber_mailbox.h broadcast_fanout() gates per attached fiber
  - multi_fiber_mailbox.h snapshot_global_full exposes new counter
  - multi_fiber_mailbox.h cites Issue #2312
  - evaluator_primitives_messaging.cpp query:mf-mailbox-stats extended with
    schema-2312 / issue-2312 / mailbox-deferred-mutation-hold-total /
    mailbox-mutation-hold-gate-wired
  - tests/serve/test_mailbox_recv_mutation_boundary_2188.cpp cites 2312

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_mailbox_mutation_hold_gate_coverage.py
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
        # multi_fiber_mailbox.h
        (
            ROOT / "src/serve/multi_fiber_mailbox.h",
            "mailbox_deferred_mutation_hold_total",
            "MultiFiberMailboxStats has new counter",
        ),
        (
            ROOT / "src/serve/multi_fiber_mailbox.h",
            "is_at_mutation_boundary_safe",
            "push/broadcast_fanout gate uses safety API",
        ),
        (ROOT / "src/serve/multi_fiber_mailbox.h", "Issue #2312", "multi_fiber_mailbox.h cites 2312"),
        (
            ROOT / "src/serve/multi_fiber_mailbox.h",
            "mutation_safety_snapshot",
            "multi_fiber_mailbox.h uses snapshot API",
        ),
        (
            ROOT / "src/serve/multi_fiber_mailbox.h",
            "deferred_mutation_hold",
            "snapshot_global_full exposes new counter",
        ),
        (
            ROOT / "src/serve/multi_fiber_mailbox.h",
            "reject_if_linear_viol",
            "linear-viol path still runs (regression-safe)",
        ),
        # fiber.h (reuses #2184 snapshot API)
        (ROOT / "src/serve/fiber.h", "is_at_mutation_boundary_safe", "fiber.h exposes safety API (#2184)"),
        (ROOT / "src/serve/fiber.h", "mutation_safety_snapshot", "fiber.h exposes snapshot API (#2184)"),
        # evaluator_primitives_messaging.cpp query primitive
        (
            ROOT / "src/compiler/evaluator_primitives_messaging.cpp",
            "mailbox-deferred-mutation-hold-total",
            "query primitive exposes suppress counter",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_messaging.cpp",
            "mailbox-mutation-hold-gate-wired",
            "query primitive gate-wired sentinel",
        ),
        (ROOT / "src/compiler/evaluator_primitives_messaging.cpp", "schema-2312", "query primitive schema-2312"),
        (ROOT / "src/compiler/evaluator_primitives_messaging.cpp", "issue-2312", "query primitive issue-2312"),
        (
            ROOT / "src/compiler/evaluator_primitives_messaging.cpp",
            "&def_mh",
            "query primitive passes deferred_mutation_hold out-param",
        ),
        # test file
        (ROOT / "tests/serve/test_mailbox_recv_mutation_boundary_2188.cpp", "Issue #2312", "test file cites 2312"),
        (
            ROOT / "tests/serve/test_mailbox_recv_mutation_boundary_2188.cpp",
            "ac2312_push_deferred_under_guard",
            "test file has #2312 AC1 push defer test",
        ),
        (
            ROOT / "tests/serve/test_mailbox_recv_mutation_boundary_2188.cpp",
            "ac2312_source_and_regression",
            "test file has #2312 AC2/AC3 source + regression test",
        ),
        # Linter self-reference (sanity)
        (
            ROOT / "scripts/check_mailbox_mutation_hold_gate_coverage.py",
            "delivery gate on target MutationBoundary",
            "linter self-reference",
        ),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2312 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
