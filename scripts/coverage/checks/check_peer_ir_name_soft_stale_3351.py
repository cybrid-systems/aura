#!/usr/bin/env python3
"""Issue #3351: owner-scoped peer IR-cache must not clean-hit.

#3136 clears residual_force on local restamp. #3070/#3300 cover AOT/JIT
peer soft-stale. Residual: lookup_define_v2 / should_relower ignore the
name-level peer authority, so a peer can serve pre-invalidate IR.

Fix: name-level IR gen (sibling of #3300) marked from
hard_invalidate_via_facade when epoch did not move. lookup_define_v2
last-looks gen > entry.peer_ir_stale_ack_ before a clean hit. Local
restamp acks. No g_aot_table_epoch bump. Soft/empty/single-eval: one
acquire. No new query key.

Contract:
  AC1 lookup last-look + facade mark with #3300
  AC2 Soft/empty/single-eval 0 extra; #3300/#3136 retained
  AC3 production mark → lookup not clean; restamp acks
  AC4 after #3229; no invent / docs/design / g_3351_* / schema-3351

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

    svc = _read("src/compiler/service.ixx")
    dirty = _read("src/compiler/service_dirty.cpp")
    bridge = _read("src/compiler/aura_jit_bridge.cpp")
    header = _read("src/compiler/aura_jit_bridge.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    reg = _read("src/compiler/hot_update_registry.cpp")
    hh = _read("src/compiler/hot_update_registry.hh")
    test = _read("tests/compiler/test_peer_jit_name_soft_stale.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp") + _read("src/compiler/evaluator_primitives_obs_eval.cpp")

    must("kPeerIrNameSoftStaleIssue = 3351", "AC1 stamp", hh)
    must("aura_aot_mark_peer_ir_name_soft_stale", "AC1 mark", bridge)
    must("aura_aot_peer_ir_name_stale_gen", "AC1 gen", header)
    must("peer_ir_stale_ack_", "AC1 ack field", svc)
    must("aura_aot_mark_peer_ir_name_soft_stale(name)", "AC1 facade", reg)
    must("ac3351_1", "AC1 test", test)

    look = svc.find("int lookup_define_v2")
    win = svc[look : look + 8000] if look >= 0 else ""
    gen = win.find("aura_aot_peer_ir_name_stale_gen")
    hit = win.find("return 0; // hit")
    if look < 0:
        fails.append("AC1: lookup_define_v2 missing")
    elif gen < 0 or hit < 0 or gen > hit:
        fails.append("AC1: peer IR gen last-look must precede clean-hit return")
    must("ack_peer_ir_stale_on_restamp_", "AC1 restamp ack", svc)
    must("ack_peer_ir_stale_on_restamp_", "AC1 cascade ack", dirty)

    must("aura_aot_state_map_size() <= 1", "AC2 single-eval skip", bridge)
    must("g_peer_ir_name_soft_stale_live", "AC2 empty probe", bridge)
    must("ac3351_2_soft_quiet", "AC2 test", test)
    must("aura_aot_mark_peer_jit_name_soft_stale", "AC2 #3300 retained", reg)
    must("note_relower_success_define", "AC2 #3136/#3229 retained", svc)

    must("ac3351_3", "AC3 test", test)
    must("g_aot_table_epoch", "AC3 epoch preserved cite", test)
    if "schema-3351" in q or "schema-3351" in hh:
        fails.append("AC3: new schema-3351 query key")
    if "g_3351_" in bridge or "g_3351_" in svc:
        fails.append("AC3: new g_3351_* counter")

    must("check_peer_ir_name_soft_stale_3351", "AC4 build.py", build)
    must("check_relower_success_define_collision_3229", "AC4 after #3229", build)
    i3229 = build.find("check_relower_success_define_collision_3229.py")
    i3351 = build.find("check_peer_ir_name_soft_stale_3351.py")
    if i3229 < 0 or i3351 < 0 or i3351 < i3229:
        fails.append("AC4: #3351 linter must run after #3229")
    must("ac3351_4_linter_no_invent", "AC4 test", test)
    must("aura_aot_mark_peer_ir_name_soft_stale", "AC4 stub", stub)
    if (ROOT / "tests" / "issues" / "test_issue_3351.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3351.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3351.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3351.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3351-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3351 peer_ir_name_soft_stale:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3351 peer_ir_name_soft_stale: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
