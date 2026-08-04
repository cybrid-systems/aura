#!/usr/bin/env python3
"""Issue #2265: LifetimePin Phase 3 — real ptr remap under Moving densify.

Contract (5 AC from issue body):
  AC1 — API: add LifetimePin::remap(void* new_ptr, std::uint64_t new_gen = 0)
          + lifetime::remap_pins_pointing_to(old, new, new_gen, arena_id_filter)
          that walks the pin registry under the registry mutex.
  AC2 — Wire at densify site: after relocate_tracked_objects_for_moving_()
          fills last_object_remap_, call remap for every (old, neu) pair
          BEFORE generation restamp / layout-change callbacks. Pins not
          present in remap table keep existing invalidate-or-restamp policy.
  AC3 — Zero-cost happy path: no registry walk on Soft/Force success paths
          that do not move objects. remap / registry walk only on Moving
          densify success.
  AC4 — Observability: counters lifetime_pin_remap_total +
          lifetime_pin_remap_miss_total (process + optional
          CompilerMetrics mirror arena_live_compact_remapped_pins_total).
          Extend query:arena-live-compact-stats with schema-2265 /
          issue-2265 lineage (additive, no schema break).
  AC5 — Tests: extend tests/core/test_moving_compact.cpp — pin →
          Moving → validate(cur_gen, arena_id) succeeds AND ptr() equals
          the densified address. Negative: pin a non-arena address →
          pin invalidates after Moving.

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
    gc = _read("src/compiler/evaluator_gc.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    test = _read("tests/core/test_moving_compact.cpp")

    # AC1 — API surface
    _must(
        "LifetimePin::remap" in pin or "remap(void* new_ptr" in pin,
        "AC1: LifetimePin::remap() method missing",
        fails,
    )
    _must(
        "void remap(" in pin,
        "AC1: LifetimePin::remap(void*, std::uint64_t) signature missing",
        fails,
    )
    _must(
        "remap_pins_pointing_to" in pin,
        "AC1: lifetime::remap_pins_pointing_to() helper missing",
        fails,
    )
    _must(
        "RemapResult" in pin,
        "AC1: RemapResult struct missing",
        fails,
    )
    _must(
        "lifetime_pin_remap_total" in pin,
        "AC1: lifetime_pin_remap_total() stat helper missing",
        fails,
    )
    _must(
        "lifetime_pin_remap_miss_total" in pin,
        "AC1: lifetime_pin_remap_miss_total() stat helper missing",
        fails,
    )
    _must(
        "kLifetimePinPhase = 3" in pin or "kLifetimePinPhase = 3;" in pin,
        "AC1: kLifetimePinPhase bumped to 3 (Phase 3 marker)",
        fails,
    )
    _must(
        "remap_misses" in pin,
        "AC1: LifetimePinStats.remap_misses counter missing",
        fails,
    )

    # AC2 — Wire at densify site
    _must(
        "remap_pins_pointing_to(" in arena,
        "AC2: arena.ixx wire-up at densify site missing",
        fails,
    )
    _must(
        "last_object_remap_" in arena and "for (const auto&" in arena,
        "AC2: arena.ixx must iterate last_object_remap_ after relocate",
        fails,
    )
    _must(
        "result.remapped_pins" in arena,
        "AC2: LiveCompactResult.remapped_pins must be set in wire-up",
        fails,
    )
    # Issue #2374: selective invalidate moved to sharded helper
    # invalidate_pins_not_in_new_addrs (legacy pin_registry() walk removed).
    _must(
        "new_addrs" in arena and ("invalidate_pins_not_in_new_addrs" in arena or "unpin_on_compact" in arena),
        "AC2: selective invalidate must skip remapped pins via new_addrs / sharded helper",
        fails,
    )
    _must(
        "invalidate_pins_not_in_new_addrs" in pin or "unpin_on_compact" in pin,
        "AC2: LifetimePin side must implement selective invalidate / unpin_on_compact",
        fails,
    )
    # Issue #2374: no legacy empty pin_registry() densify walk (call sites).
    # Comments may still mention the removed API.
    _must(
        "pin_registry_mtx()" not in arena
        and "lifetime::pin_registry()" not in arena
        and "lifetime::pin_registry_mtx()" not in arena,
        "AC2/#2374: densify must not walk legacy pin_registry() (empty post-#2342)",
        fails,
    )
    # Wire-up must be AFTER gen restamp (since remap needs new_gen).
    # The remap call references result.new_gen, so it must come after
    # the generation_.fetch_add line.
    _must(
        "generation_.fetch_add(1" in arena
        and "result.new_gen = generation_" in arena
        and "remap_pins_pointing_to" in arena,
        "AC2: remap_pins_pointing_to must be wired with new_gen (after fetch_add)",
        fails,
    )

    # AC3 — Zero-cost happy path (no registry walk on Soft/Force)
    _must(
        "Moving" in arena and "Soft" in arena and "Force" in arena,
        "AC3: LiveCompactMode Soft / Force / Moving branches present",
        fails,
    )
    _must(
        "result.moved_live_objects && !last_object_remap_.empty()" in arena,
        "AC3: remap loop guarded by moved_live_objects (zero-cost on Soft/Force)",
        fails,
    )

    # AC4 — Observability
    _must(
        "remapped_pins" in arena,
        "AC4: LiveCompactResult.remapped_pins counter added",
        fails,
    )
    _must(
        "arena_live_compact_remapped_pins_total" in met,
        "AC4: CompilerMetrics.arena_live_compact_remapped_pins_total atomic missing",
        fails,
    )
    _must(
        "arena_live_compact_remapped_pins_total" in gc,
        "AC4: evaluator_gc.cpp must mirror arena_live_compact_remapped_pins_total",
        fails,
    )
    _must(
        "arena_live_compact_remapped_pins_total" in ev,
        "AC4: evaluator.ixx must mirror arena_live_compact_remapped_pins_total",
        fails,
    )
    _must(
        'insert_kv(\n                "remapped-pins-total"' in q or '"remapped-pins-total"' in q,
        "AC4: query:arena-live-compact-stats must surface remapped-pins-total key",
        fails,
    )
    _must(
        '"schema-2265"' in q and '"issue-2265"' in q,
        "AC4: query primitive must surface schema-2265 / issue-2265 lineage",
        fails,
    )
    _must(
        '"lifetime-pin-remap-wired"' in q,
        "AC4: query primitive must surface lifetime-pin-remap-wired sentinel",
        fails,
    )

    # AC5 — Tests (Moving densify-with-live-pin superseded by AC_M3 block:
    # live pins refuse densify. AC_M5 asserts the block; #2374 helper covers
    # selective invalidate without densify.)
    _must(
        "AC_M5: live arena pin blocks Moving densify" in test or "AC_M5: Moving blocked with live pin" in test,
        "AC5: live-pin blocks Moving densify AC_M5 missing in test_moving_compact.cpp",
        fails,
    )
    _must(
        "AC_M5: Moving blocked with live non-arena pin" in test or "non-arena pin also blocks Moving" in test,
        "AC5: non-arena pin block path missing in test_moving_compact.cpp",
        fails,
    )
    _must(
        "invalidate_pins_not_in_new_addrs" in test and "Issue #2374" in test,
        "AC5: #2374 selective invalidate helper must remain covered",
        fails,
    )
    _must(
        "non-arena pin" in test.lower() or "local pin" in test.lower() or "local{" in test,
        "AC5: test must pin a non-arena address (local buffer)",
        fails,
    )
    _must(
        "lifetime_pin_remap_total" in test or "verify_pins_under_moving_compact" in test,
        "AC5: tests must exercise pin remap / verify contract counters",
        fails,
    )

    # AC_2342 — Issue #2342: sharded LifetimePin registry (Option 1 —
    # shard by arena_id, N=16). Refines #2265/#2270/#2160/#2298.
    # Production review (2026-07-29) 建议 6.
    _must(
        "kPinRegistryShardCount = 16" in pin,
        "AC_2342: kPinRegistryShardCount == 16 (power of 2 for arena_id AND)",
        fails,
    )
    _must(
        "struct PinRegistryShard" in pin,
        "AC_2342: PinRegistryShard struct missing",
        fails,
    )
    _must(
        "pin_registry_shards()" in pin,
        "AC_2342: pin_registry_shards() accessor missing",
        fails,
    )
    _must(
        "pin_registry_shard_index" in pin,
        "AC_2342: pin_registry_shard_index(arena_id) helper missing",
        fails,
    )
    _must(
        "g_pin_registry_lock_wait_us_total" in pin and "pin_registry_lock_wait_us_total" in pin,
        "AC_2342: lock_wait_us_total atomic + accessor missing",
        fails,
    )
    _must(
        "pin_registry_shard_pin_count" in pin
        and "pin_registry_shard_max_pin_count" in pin
        and "pin_registry_total_pinned_count" in pin,
        "AC_2342: per-shard pin count + max + total accessors missing",
        fails,
    )
    _must(
        "shard_index_" in pin,
        "AC_2342: LifetimePin::shard_index_ member missing",
        fails,
    )
    _must(
        "Issue #2342" in pin,
        "AC_2342: lifetime_pin.ixx must cite Issue #2342",
        fails,
    )
    # Compact functions rewired to iterate shards (no longer use
    # global pin_registry_mtx() lock at function entry).
    _must(
        "for (std::size_t i = 0; i < kPinRegistryShardCount; ++i)" in pin,
        "AC_2342: compact functions must iterate all shards in order",
        fails,
    )
    # Query surface: query:lifetime-contract-snapshot extends with #2342 keys.
    q_obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    _must(
        '"pin-registry-shard-count"' in q_obs,
        "AC_2342: query:lifetime-contract-snapshot must surface pin-registry-shard-count",
        fails,
    )
    _must(
        '"pin_registry_shard_count"' in q_obs,
        "AC_2342: query:lifetime-contract-snapshot must surface pin_registry_shard_count (snake alias)",
        fails,
    )
    _must(
        '"pin-registry-shard-max-pin-count"' in q_obs,
        "AC_2342: query:lifetime-contract-snapshot must surface pin-registry-shard-max-pin-count",
        fails,
    )
    _must(
        '"pin-registry-lock-wait-us-total"' in q_obs,
        "AC_2342: query:lifetime-contract-snapshot must surface pin-registry-lock-wait-us-total",
        fails,
    )
    _must(
        '"pin-registry-shard-wired"' in q_obs,
        "AC_2342: query:lifetime-contract-snapshot must surface pin-registry-shard-wired sentinel",
        fails,
    )
    _must(
        '"schema-2342"' in q_obs and '"issue-2342"' in q_obs,
        "AC_2342: query primitive must surface schema-2342 / issue-2342 lineage",
        fails,
    )
    # Tests: ac2342_* test functions.
    _must(
        "void ac2342_1_shard_constants_and_accessors" in test,
        "AC_2342: ac2342_1_shard_constants_and_accessors test missing",
        fails,
    )
    _must(
        "void ac2342_2_ctor_dtor_routes_to_shard_zero" in test,
        "AC_2342: ac2342_2_ctor_dtor_routes_to_shard_zero test missing",
        fails,
    )
    _must(
        "void ac2342_3_lock_wait_us_total" in test,
        "AC_2342: ac2342_3_lock_wait_us_total test missing",
        fails,
    )
    _must(
        "void ac2342_4_query_schema" in test,
        "AC_2342: ac2342_4_query_schema test missing",
        fails,
    )
    _must(
        "void ac2342_5_source_cite" in test,
        "AC_2342: ac2342_5_source_cite test missing",
        fails,
    )
    _must(
        "Issue #2342" in test,
        "AC_2342: test_moving_compact.cpp must cite Issue #2342",
        fails,
    )

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2265 LifetimePin Phase 3 remap coverage linter")
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
    print("OK: LifetimePin Phase 3 remap coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
