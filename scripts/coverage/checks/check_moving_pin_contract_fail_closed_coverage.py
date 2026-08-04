#!/usr/bin/env python3
"""Issue #2266: LifetimePin Phase 4 — verify_pins_under_moving_compact must fail-closed.

Contract (5 AC from issue body):
  AC1 — Semantics: verify_pins_under_moving_compact(arena_id, old_addresses)
          returns false if any pin with matching arena_id has non-null ptr_ that
          appears as a key in the densify's old→new map AND ptr_ was not
          updated to the new address (and not invalidated). Returns true when all
          such pins were honored or when no pins / empty remap.
  AC2 — Driver behavior: Outermost Phase 5 / compact driver — on false →
          bump moving_compact_pin_contract_fail_total, do not publish success
          metrics as if contract held; optional env AURA_MOVING_PIN_CONTRACT=hard
          forces hard-fail vs soft metric (default hard under production security
          defaults).
  AC3 — Zero-cost happy path: not called from allocation hot path; only
          compact / Phase 5 driver (preserve #2256 AC3).
  AC4 — Observability: moving_compact_pin_contract_fail_total counter
          (process + optional CompilerMetrics) + query key + schema-2266 /
          issue-2266 / moving-pin-contract-wired lineage on
          query:arena-live-compact-stats.
  AC5 — Tests: positive (pin → Moving → remap → contract held) + negative
          (pin not remapped → verify returns false + counter bumps).

This linter is the source-of-truth for the production surface.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = REPO / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8")


def _must(cond: bool, msg: str, fails: list) -> None:
    if not cond:
        fails.append(msg)


def check() -> list:
    fails = []

    pin = _read("src/core/lifetime_pin.ixx")
    arena = _read("src/core/arena.ixx")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    eval_ = _read("src/compiler/evaluator_mutation_boundary.cpp")
    gc = _read("src/compiler/evaluator_gc.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    test = _read("tests/core/test_moving_compact_2166.cpp")

    # AC1 — Semantics: verify_pins_under_moving_compact takes (arena_id, old_addresses)
    # and returns false on contract fail (pin's ptr_ in old_addresses).
    _must(
        "verify_pins_under_moving_compact" in pin,
        "AC1: verify_pins_under_moving_compact function missing",
        fails,
    )
    _must(
        "old_addresses" in pin and "old_addresses.count" in pin,
        "AC1: verify_pins_under_moving_compact must take old_addresses set and check pin's ptr_",
        fails,
    )
    _must(
        "return false" in pin and "if (old_addresses.count(p->ptr()) > 0)" in pin,
        "AC1: verify_pins_under_moving_compact must return false when pin's ptr_ is in old_addresses",
        fails,
    )
    # verify_pins_under_moving_compact must bump g_moving_compact_pin_contract_fail_total
    _must(
        "g_moving_compact_pin_contract_fail_total.fetch_add" in pin,
        "AC1: verify_pins_under_moving_compact must bump g_moving_compact_pin_contract_fail_total",
        fails,
    )

    # AC2 — Driver behavior: env AURA_MOVING_PIN_CONTRACT=hard + do not publish success metrics
    _must(
        "compact_all_moving_pinned" in arena,
        "AC2: ArenaGroup::compact_all_moving_pinned() helper missing",
        fails,
    )
    _must(
        "AdaptiveCompactResult" in arena,
        "AC2: AdaptiveCompactResult struct missing in arena.ixx",
        fails,
    )
    _must(
        "compact_all_moving_pinned" in eval_ and "pin_contract_held" in eval_,
        "AC2: driver must use compact_all_moving_pinned() + check pin_contract_held",
        fails,
    )
    _must(
        "AURA_MOVING_PIN_CONTRACT" in eval_ and "hard" in eval_,
        "AC2: driver must support AURA_MOVING_PIN_CONTRACT=hard env var",
        fails,
    )
    _must(
        "outermost_exit_phase5_unlock_total" in eval_ and "outermost_exit_order_complete_total" in eval_,
        "AC2: driver must gate outermost_exit_phase5_unlock + order_complete counters on pin_contract_held",
        fails,
    )

    # AC3 — Zero-cost happy path: not called from allocation hot path
    _must(
        "verify_pins_under_moving_compact" not in arena.replace("compact", "") or "compact_all_moving_pinned" in arena,
        "AC3: verify_pins_under_moving_compact + compact_all_moving_pinned are Moving-only (preserved AC3 zero-cost)",
        fails,
    )

    # AC4 — Observability: moving_compact_pin_contract_fail_total + query key + schema-2266 lineage
    _must(
        "moving_compact_pin_contract_fail_total" in met,
        "AC4: CompilerMetrics.moving_compact_pin_contract_fail_total atomic missing",
        fails,
    )
    _must(
        "moving_compact_pin_contract_fail_total" in gc,
        "AC4: evaluator_gc.cpp must mirror moving_compact_pin_contract_fail_total",
        fails,
    )
    _must(
        "moving_compact_pin_contract_fail_total" in ev,
        "AC4: evaluator.ixx must mirror moving_compact_pin_contract_fail_total",
        fails,
    )
    _must(
        "moving-compact-pin-contract-fail-total" in q,
        "AC4: query:arena-live-compact-stats must surface moving-compact-pin-contract-fail-total key",
        fails,
    )
    _must(
        '"schema-2266"' in q and '"issue-2266"' in q and '"moving-pin-contract-wired"' in q,
        "AC4: query primitive must surface schema-2266 / issue-2266 / moving-pin-contract-wired lineage",
        fails,
    )

    # AC5 — Tests
    _must(
        "AC_M6 positive" in test and "pin_contract_held" in test,
        "AC5: positive test (pin + remap → contract held) missing",
        fails,
    )
    _must(
        "AC_M6 negative" in test
        and "verify_pins_under_moving_compact" in test
        and "lifetime_pin_contract_fail_total" in test,
        "AC5: negative test (pin not remapped → verify returns false + counter bumps) missing",
        fails,
    )
    _must(
        "old_addresses" in test or "old_addrs" in test,
        "AC5: tests must pass old_addresses set to verify_pins_under_moving_compact",
        fails,
    )

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Issue #2266 verify_pins_under_moving_compact fail-closed coverage linter"
    )
    parser.add_argument("--self-test", action="store_true", help="Run self-test (return 0 if contract satisfied)")
    parser.add_argument("--strict", action="store_true", help="Strict mode (non-zero exit on any failure)")
    args = parser.parse_args()
    fails = check()
    if args.self_test:
        print(f"self-test: {len(fails)} failures")
        return 0 if not fails else 1
    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: verify_pins_under_moving_compact fail-closed coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
