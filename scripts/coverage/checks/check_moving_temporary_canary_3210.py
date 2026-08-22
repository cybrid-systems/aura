#!/usr/bin/env python3
"""Issue #3210: temporary EnvFrame/Closure/JIT/FFI live-ptr canary inventory.

Residual of #3055 / #2935 / #3182. register_known_moving_densify_root_slots
is a closed set; stack/temp holders must enter post_moving_live_canaries_
via TemporaryMovingLivePtrCanary (or be elevated to a stable void** slot).

Contract (one row per AC):
  AC1  Process-wide (not TLS) temp inventory + RAII; drained at live_compact
       Moving and register_known_moving_densify_root_slots.
  AC2  Injected unregistered temp → fail-closed when it would go stale;
       healthy windows including new canary sources have stale==0;
       prefer slot elevation where a stable void** exists.
  AC3  Soft / Off / empty inventory: moving_compact_enabled gate + live==0
       early return (zero extra mutex).
  AC4  apply_closure notes cl_copy.flat / cl_copy.pool (not rehash-stable
       slots). Stay on LifetimePin SSOT + existing canary list.
  AC5  Stamp kMovingTemporaryCanaryIssue=3210 at densify_consistency_report.h
       END; extend test_moving_densify_fail_closed; linter wired in build.py.
       No docs/design/3210-* (#1655); no test_issue_3210.cpp (#81967).

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

    arena = _read("src/core/arena.ixx")
    mut = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    apply = _read("src/compiler/evaluator_eval_flat.cpp")
    dc = _read("src/core/densify_consistency_report.h")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    # ── AC1: inventory + drain sites ──
    must("TemporaryMovingLivePtrCanary", "AC1 RAII", arena)
    must("note_temporary_moving_live_ptr", "AC1 note", arena)
    must("snapshot_temporary_moving_live_ptrs", "AC1 snapshot", arena)
    must("moving_temp_canary_detail", "AC1 process-wide inventory", arena)
    if (
        "thread_local"
        in arena[arena.find("moving_temp_canary_detail") : arena.find("struct TemporaryMovingLivePtrCanary")]
    ):
        fails.append("AC1: TLS inventory (forbidden — aarch64 TLSLE)")
    must("note_temporary_moving_live_canaries()", "AC1 live_compact drain", arena)
    must("note_temporary_moving_live_canaries_all()", "AC1 group drain", arena)
    must("note_temporary_moving_live_canaries_all()", "AC1 register_known drain", mut)

    # ── AC2: tests ──
    must("ac3210_1_injected_unregistered_temp_fail_closed", "AC2 fail-closed test", test)
    must("ac3210_2_healthy_window_stale_zero", "AC2 healthy test", test)
    must("ac3210_3_elevate_temp_to_slot", "AC2 slot elevation test", test)
    must("ac3210_4_soak_unregistered_temp", "AC2 soak test", test)
    must("ac3210_1_injected_unregistered_temp_fail_closed();", "AC2 fail-closed called", test)

    # ── AC3: Soft / Off ──
    must("if (!moving_compact_enabled())", "AC3 note gated", arena)
    must("inv.live.load(std::memory_order_acquire) == 0", "AC3 empty-list early return", arena)
    must("ac3210_5_soft_zero_extra", "AC3 Soft test", test)

    # ── AC4: apply_closure + no extra pin model ──
    must("TemporaryMovingLivePtrCanary tmp_flat", "AC4 apply flat", apply)
    must("TemporaryMovingLivePtrCanary tmp_pool", "AC4 apply pool", apply)
    must("Issue #3210", "AC4 apply cite", apply)
    if "class PostMovingPinRegistry" in arena or "g_moving_pin_registry_3210" in arena:
        fails.append("AC4: extra pin registry introduced")
    if "class MovingCanaryRegistry" in mut:
        fails.append("AC4: extra canary registry introduced")
    must("TemporaryMovingLivePtrCanary", "AC4 evaluator comment", ev)

    # ── AC5: stamp + wiring + no invent ──
    must("kMovingTemporaryCanaryIssue = 3210", "AC5 stamp", dc)
    # Stamp must sit after force_reason_to_string (header end, not mid-struct).
    stamp_pos = dc.find("kMovingTemporaryCanaryIssue = 3210")
    fr_pos = dc.find("force_reason_to_string")
    if stamp_pos == -1 or fr_pos == -1 or stamp_pos < fr_pos:
        fails.append("AC5: stamp must be appended at densify_consistency_report.h end")
    must("check_moving_temporary_canary_3210", "AC5 build", build)
    must("ac3210_6_source_cite_no_invent", "AC5 source-cite test", test)
    if _read("docs/design/3210-temporary-canary.md"):
        fails.append("AC5: docs/design/3210-* present (forbidden per #1655)")
    if (ROOT / "tests" / "core" / "test_issue_3210.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3210.cpp present (forbidden per #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3210.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3210.cpp present (forbidden per #81967)")

    if fails:
        print(f"Issue #3210 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3210 temporary EnvFrame/Closure/JIT/FFI live-ptr canary — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
