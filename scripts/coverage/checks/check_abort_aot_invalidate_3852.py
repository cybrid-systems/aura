#!/usr/bin/env python3
"""Issue #3852: production abort IR fence also invalidates AOT.

Dual-topology abort force-dirties / drops IR cache so lookup needs-relower,
but left AOT table epoch / slots live — aura_aot_probe_fn_ptr could return
mid-mutate native. Production abort force-dirty now force-bumps the AOT
table epoch (SSOT). Soft / Off: AOT untouched.

Contract (one row per AC):
  AC1  production force_ir_cache_dirty_after_abort force-bumps + bumps
       AOT table epoch (same window as IR fence)
  AC2  Soft / Off path does not call AOT bump (production_defaults gate)
  AC3  probe still gates on table epoch / soft_stale (no abort_force invent)
  AC4  extends test_abort_ir_cache_fence_first; linter + grandfather +
       manifest + build.py; no invent / docs/design; no schema-3852 /
       g_3852_* production counters

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SVC = "src/compiler/service.ixx"
BRIDGE = "src/compiler/aura_jit_bridge.cpp"
TEST = "tests/compiler/test_abort_ir_cache_fence_first.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/3852.json"
LINTER = "check_abort_aot_invalidate_3852"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    svc = _read(SVC)
    bridge = _read(BRIDGE)
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    start = svc.find("void force_ir_cache_dirty_after_abort()")
    if start < 0:
        fails.append("AC1: force_ir_cache_dirty_after_abort missing")
        body = ""
    else:
        # Body ends at next top-level method after the closing brace window.
        end = svc.find("\n    [[nodiscard]] bool gate_partial_soa_dirty_sync_", start)
        if end < 0:
            end = start + 3500
        body = svc[start:end]
        must("Issue #3852", "AC1 cite", body)
        must("aura_aot_note_cross_eval_epoch_force_bump()", "AC1 force-bump note", body)
        must("aura_aot_bump_func_table_epoch()", "AC1 table epoch bump", body)
        must("production_defaults_active()", "AC1 production gate", body)
        # Bump must land before in_progress clear (same abort window).
        bump = body.find("aura_aot_bump_func_table_epoch()")
        clear = body.find("abort_force_in_progress_.store(0, std::memory_order_release)")
        if not (0 <= bump < clear):
            fails.append("AC1: AOT bump must precede abort_force_in_progress_ clear")
        # Soft gate: AOT calls only inside production/Full branch.
        gate = body.find("production_defaults_active()")
        if gate < 0:
            fails.append("AC2: production_defaults_active gate missing")
        else:
            # Find the if that wraps the bump — Soft must not call bump outside.
            win = body[gate : gate + 600]
            must("aura_aot_note_cross_eval_epoch_force_bump()", "AC2 bump inside gate", win)
            must("aura_aot_bump_func_table_epoch()", "AC2 epoch inside gate", win)
            must("AuditStrategy::Full", "AC2 Full strategy arm", win)

    # Soft observe-only cite retained on clear_cache_v2 (IR); AOT skip is the gate.
    must("clear_cache_v2_for_define", "AC2 IR Soft observe helper kept", svc)

    # Probe gates on epoch / soft_stale — do not invent abort_force on probe.
    pstart = bridge.find("extern \"C\" std::uintptr_t aura_aot_probe_fn_ptr(")
    if pstart < 0:
        fails.append("AC3: aura_aot_probe_fn_ptr missing")
        pwin = ""
    else:
        pend = bridge.find("extern \"C\" std::uintptr_t aura_aot_probe_fn_ptr_raw(", pstart)
        if pend < 0:
            pend = pstart + 1200
        pwin = bridge[pstart:pend]
        must("g_aot_table_epoch", "AC3 epoch gate", pwin)
        must("soft_stale", "AC3 soft_stale gate", pwin)
        must_not("abort_force_generation", "AC3 no abort_force invent on probe", pwin)
        must_not("g_3852_", "AC3 no invented counter", pwin)

    must("ac3852_1_production_abort_probe_rejects_until_reemit", "AC4 test AC1", test)
    must("3852 AC1: probe rejects mid-abort native", "AC4 assert probe", test)
    must("ac3852_2_soft_abort_aot_untouched", "AC4 test Soft", test)
    must("3852 AC3: Soft abort does not bump AOT table epoch", "AC4 Soft assert", test)
    must("ac3852_3_source_and_linter", "AC4 test wiring", test)

    must(LINTER, "AC4 build registration", build)
    must("Issue #3852", "AC4 build cite", build)
    must(f"{LINTER}.py", "AC4 grandfather basename", gf)
    must(f"scripts/coverage/checks/{LINTER}.py", "AC4 grandfather path", gf)
    must('"issue": 3852', "AC4 manifest issue", man)
    if not (ROOT / MANIFEST).is_file():
        fails.append(f"AC4: {MANIFEST} missing")

    must_not("schema-3852", "AC4 no query key", svc)
    must_not("g_3852_", "AC4 no g_3852_* production", svc)
    must_not("g_3852_", "AC4 no g_3852_* bridge", bridge)

    for rel in (
        "tests/compiler/test_issue_3852.cpp",
        "tests/issues/test_issue_3852.cpp",
        "docs/design/3852-abort-aot-invalidate.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC4: forbidden invent {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3852*"):
            fails.append(f"AC4: docs/design/{p.name} exists")

    # Existing abort IR fence helpers still present (do not claim missing).
    must("begin_abort_ir_cache_force_fence", "AC1 IR fence kept", svc)
    must("abort_map_invalid = true", "AC1 abort_map_invalid kept", body if body else svc)

    if fails:
        print("FAIL #3852 abort_aot_invalidate:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print(
        "OK #3852 abort_aot_invalidate: production abort force-bumps AOT; "
        "Soft untouched; probe epoch/soft_stale gates"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
