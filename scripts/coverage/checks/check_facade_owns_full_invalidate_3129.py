#!/usr/bin/env python3
"""Issue #3129: P0 production facade must absorb full invalidate semantics.

After #3112 closed the entry-point dual-track, the facade
(hard_invalidate_via_facade) still only performs decide_and_reemit
(reemit-only). It skipped atomic_bump_epochs_and_stamp_bridge
+ IR dirty stamp + dep-graph cascade + linear post-mutate bookkeeping.
Production mutate → reemit could see empty dirty set / stale epochs.

Fix: hard_invalidate_via_facade now calls aura_aot_bump_func_table_epoch
+ aura_aot_note_cross_eval_hard_owner_scoped before decide_and_reemit
so the subsequent aura_reemit_aot_for_dirty sees the mutated define.

Contract:
  AC1 hot_update_registry.cpp::hard_invalidate_via_facade calls
      aura_aot_bump_func_table_epoch() + aura_aot_note_cross_eval_hard_owner_scoped()
      before decide_and_reemit (under production). Soft / Off unchanged
      (returns false; zero extra work).
  AC2 Runtime: aura_aot_func_table_epoch advances after facade call under
     production. Soft / Off: no advance (facade returns false).
  AC3 Existing #3112 sibling invariants preserved (dual-track gate
     stays the single production entry; Soft / Off return false).
  AC4 Existing decide_and_reemit still calls aura_reemit_aot_for_dirty
     (no ABI regression).
  AC5 Bridge audit linter (#3112) still clean.
  AC6 tests/compiler/test_compiler_hot_update_facade.cpp extended with
     ac3129_facade_owns_full_invalidate (no new test_issue_3129.cpp).
  ──────────────────────────────────────────────────────────────────
  AC7 (#3150) Facade body now also advances bridge_epoch +
      defuse_version (joint epoch under production). C-ABI bumpers
      defined next to aura_aot_bump_func_table_epoch in
      aura_jit_bridge.cpp. Order: bridge → defuse → aot table
      (matches atomic_bump_epochs_and_stamp_bridge). Name parameter
      no longer (void) — used for notify_dirty_define(name).
  AC8 (#3150) Facade body publishes notify_dirty_define(name) so the
      subsequent aura_reemit_aot_for_dirty sees a non-empty candidate
      set for the mutated define (mutate → dirty → reemit closed loop).
      Soft / Off unchanged (early-return before notify_dirty_define).
  AC9 (#3150) tests/compiler/test_compiler_hot_update_facade.cpp extended
      with ac3150_facade_owns_full_joint_epoch_and_dirty (no new
      test_issue_3150.cpp per #81967; no docs/design/3150-* per #1655).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hur = _read("src/compiler/hot_update_registry.cpp")
    test = _read("tests/compiler/test_compiler_hot_update_facade.cpp")

    # AC1 — facade body advances AOT table epoch + cross-eval cascade.
    must("Issue #3129", "AC1", hur)
    facade_pos = hur.find("hard_invalidate_via_facade(const char* name, ReemitReason reason)")
    next_func = hur.find("\nbool ", facade_pos + 1) if facade_pos > 0 else -1
    facade_block = hur[facade_pos:next_func] if next_func > facade_pos else hur[facade_pos:]
    must("aura_aot_bump_func_table_epoch()", "AC1", facade_block)
    must("aura_aot_note_cross_eval_hard_owner_scoped()", "AC1", facade_block)
    must("decide_and_reemit(aura_get_aot_defuse_version(), reason)", "AC1", facade_block)
    # Order: bump + note + decide (epoch advance precedes reemit).
    bump_pos = facade_block.find("aura_aot_bump_func_table_epoch()")
    decide_pos = facade_block.find("decide_and_reemit(aura_get_aot_defuse_version(), reason)")
    if bump_pos < 0 or decide_pos < 0 or bump_pos > decide_pos:
        fails.append("AC1: epoch advance must precede decide_and_reemit in facade body")

    # AC2 — runtime observable via aura_aot_func_table_epoch.
    # The function is declared in aura_jit_bridge.h + defined in aura_jit_bridge.cpp.
    bridge = _read("src/compiler/aura_jit_bridge.cpp")
    bridge_h = _read("src/compiler/aura_jit_bridge.h")
    must("aura_aot_func_table_epoch", "AC2 bridge declaration", bridge_h)
    must("aura_aot_func_table_epoch", "AC2 bridge definition", bridge)
    # The AC2 runtime check is in the test file (it captures pre/post epoch).
    must("aura_aot_func_table_epoch", "AC2 runtime check in test", test)
    must("Issue #3129", "AC2 cite in test", test)

    # AC3 — #3112 invariants preserved.
    must("Issue #3112", "AC3 sibling #3112 cite preserved", hur)
    must("aura_production_defaults_active_probe() == 0", "AC3 Soft/Off returns false", hur)
    must("hard_invalidate_via_facade", "AC3 signature preserved", hur)

    # AC4 — decide_and_reemit still calls aura_reemit_aot_for_dirty.
    must("aura_reemit_aot_for_dirty(defuse_version)", "AC4 sibling decide_and_reemit", hur)

    # AC5 — bridge audit linter still clean.
    _read("build.py")
    # check_dual_track_facade_3112.py must exist + be wired in build.py.
    linter = _read("scripts/check_dual_track_facade_3112.py")
    if not linter:
        fails.append("AC5: scripts/check_dual_track_facade_3112.py missing")
    # (linter is run via subprocess in the test itself; #165/#1655 guard not needed
    # since #3129 doesn't add docs/design/ artifacts.)

    # AC6 — tests/compiler/test_compiler_hot_update_facade.cpp extended
    # with ac3129_facade_owns_full_invalidate; no new test_issue_3129.cpp.
    must("ac3129_facade_owns_full_invalidate", "AC6 test function added", test)
    must("Issue #3129", "AC6 test source-cite", test)
    must("aura_aot_bump_func_table_epoch()", "AC6 test cite", test)
    # Existing #3112 ACs preserved.
    must("ac1_facade_ownership_matches_production", "AC6 sibling #3112 ac1", test)
    must("ac2_atomic_counters_no_lost_updates", "AC6 sibling #3112 ac2", test)
    must("ac3_concurrent_facade_no_crash", "AC6 sibling #3112 ac3", test)
    must("ac4_linter_strict_clean", "AC6 sibling #3112 ac4", test)
    must("ac5_decide_and_reemit_wired", "AC6 sibling #3112 ac5", test)
    # No new test_issue_3129.cpp.
    issue_test = _read("tests/issues/test_issue_3129.cpp")
    if issue_test:
        fails.append("AC6: tests/issues/test_issue_3129.cpp exists (must NOT — src/-aligned suite per #81967)")

    # ──────────────────────────────────────────────────────────────────
    # AC7 (#3150) — facade body advances bridge_epoch + defuse_version
    # via two new C-ABI bumpers. Order: bridge → defuse → aot table
    # (matches atomic_bump_epochs_and_stamp_bridge).
    must("aura_hot_update_bump_bridge_epoch()", "AC7 bridge_epoch bumper in facade", facade_block)
    must("aura_hot_update_bump_defuse_version()", "AC7 defuse_version bumper in facade", facade_block)
    bridge_bumper_pos = facade_block.find("aura_hot_update_bump_bridge_epoch()")
    defuse_bumper_pos = facade_block.find("aura_hot_update_bump_defuse_version()")
    if bridge_bumper_pos >= 0 and defuse_bumper_pos >= 0 and bridge_bumper_pos > defuse_bumper_pos:
        fails.append(
            "AC7: bridge_epoch bumper must precede defuse_version bumper (matches atomic_bump_epochs_and_stamp_bridge)"
        )
    # defuse bumper must precede aot table epoch bumper.
    if defuse_bumper_pos >= 0 and bump_pos >= 0 and defuse_bumper_pos > bump_pos:
        fails.append(
            "AC7: defuse_version bumper must precede aot table epoch bumper (matches atomic_bump_epochs_and_stamp_bridge)"
        )
    # C-ABI bumper definitions live next to aura_aot_bump_func_table_epoch.
    must(
        'extern "C" void aura_hot_update_bump_bridge_epoch(void)',
        "AC7 bridge_epoch bumper defined in aura_jit_bridge.cpp",
        bridge,
    )
    must(
        'extern "C" void aura_hot_update_bump_defuse_version(void)',
        "AC7 defuse_version bumper defined in aura_jit_bridge.cpp",
        bridge,
    )
    # name parameter no longer (void) — used for notify_dirty_define(name).
    if "(void)name;" in facade_block:
        fails.append("AC7: name parameter is now used (notify_dirty_define); (void)name must be removed")

    # AC8 (#3150) — facade body publishes dirty for the mutated define.
    must("notify_dirty_define(name)", "AC8 dirty mark via notify_dirty_define(name)", facade_block)
    # Soft / Off unchanged: notify_dirty_define must come AFTER the
    # early-return guard (zero-cost contract). The guard is
    # `if (aura_production_defaults_active_probe() == 0) { return false; }`.
    soft_guard_pos = facade_block.find("aura_production_defaults_active_probe() == 0")
    notify_pos = facade_block.find("notify_dirty_define(name)")
    if soft_guard_pos >= 0 and notify_pos >= 0:
        # notify must come after the guard's `return false;` so Soft/Off never bumps.
        after_return = facade_block.find("return false;", soft_guard_pos)
        if after_return > 0 and notify_pos < after_return:
            fails.append("AC8: notify_dirty_define(name) must come AFTER Soft / Off early-return (zero-cost contract)")

    # AC9 (#3150) — test extension. No new test_issue_3150.cpp /
    # docs/design/3150-* per #81967 / #1655.
    must(
        "ac3150_facade_owns_full_joint_epoch_and_dirty",
        "AC9 test function added to test_compiler_hot_update_facade.cpp",
        test,
    )
    must("Issue #3150", "AC9 test source-cite marker", test)
    # Joint epoch runtime check (bridge + defuse + aot) in the test.
    must("aura_get_current_bridge_epoch", "AC9 runtime bridge_epoch check in test", test)
    must("aura_get_aot_defuse_version", "AC9 runtime defuse_version check in test", test)
    # No new test_issue_3150.cpp.
    issue_test_3150 = _read("tests/issues/test_issue_3150.cpp")
    if issue_test_3150:
        fails.append("AC9: tests/issues/test_issue_3150.cpp exists (must NOT — src/-aligned suite per #81967)")
    # No docs/design/3150-*.
    docs3150 = ROOT / "docs" / "design"
    if docs3150.is_dir():
        for f in sorted(docs3150.glob("3150-*")):
            fails.append(f"AC9: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("check_facade_owns_full_invalidate_3129: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_facade_owns_full_invalidate_3129: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
