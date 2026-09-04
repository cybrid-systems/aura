#!/usr/bin/env python3
"""Issue #3481: AOT/facade success must not restamp dirty pre-relower IR.

#2183 restamp-on-AOT-success made stamps look live while irs is still
pre-mutate. A later dirty-clear without store_define_v2 then clean-hit.
Split ack (peer/abort gen) from content restamp. Instr peel without AST
re-lower does not restamp. lookup stays 1 until store / true per-fn.
Soft/Off: no extra work. Abort path unchanged. Coverage stamp is not
content promotion (#3445).

Contract:
  AC1  lookup consults content_stored_this_epoch; cascade restamp gated
  AC2  instr peel acks fences, does not restamp_cache_entry_live_
  AC3  facade keeps decide_and_reemit; does not content-restamp; #3377/#3351 stay
  AC4  Soft facade still returns false; #2183 mismatch-force-relower stays
  AC5  abort still zeros stamps + clears map + abort_map_invalid
  AC6  last_reemit_success coverage-only; no new query key / invent / docs

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    ixx = _read("src/compiler/service.ixx")
    dirty = _read("src/compiler/service_dirty.cpp")
    hur = _read("src/compiler/hot_update_registry.cpp")
    mangle = _read("src/compiler/aot_mangle.h")
    pure = _read("src/compiler/ir_cache_pure.ixx")
    bnd = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/compiler/test_cache_stamp_restamp_contract.cpp")
    build = _read("build.py")

    lu = ixx.find("int lookup_define_v2(")
    lu_win = ixx[lu : lu + 2800] if lu >= 0 else ""
    must("Issue #3481", "AC1 lookup cite", lu_win)
    must("content_stored_this_epoch", "AC1 lookup latch", lu_win)
    must("abort_map_invalid", "AC1 abort latch stays", lu_win)
    must("3481 AC1: lookup stays 1 after dirty-clear without store", "AC1 test", t)

    maybe = ixx.find("bool maybe_restamp_cache_entry_content_live_")
    maybe_win = ixx[maybe : maybe + 1200] if maybe >= 0 else ""
    must("Issue #3481", "AC1 maybe_restamp cite", maybe_win)
    must("entry.dirty", "AC1 maybe_restamp dirty gate", maybe_win)
    must("abort_map_invalid", "AC1 maybe_restamp abort gate", maybe_win)
    must("content_stored_this_epoch", "AC1 maybe_restamp content gate", maybe_win)
    must("restamp_cache_entry_live_(entry)", "AC1 maybe_restamp content write", maybe_win)

    cas = dirty.find("void CompilerService::notify_hot_update_after_cascade_")
    cas_win = dirty[cas : cas + 16000] if cas >= 0 else ""
    must("ack_peer_ir_stale_on_restamp_", "AC1 cascade ack", cas_win)
    must("content_stored_this_epoch", "AC1 cascade content gate", cas_win)
    must("restamp_cache_entry_live_", "AC1 cascade restamp gated", cas_win)
    must("Issue #3481", "AC1 cascade cite", cas_win)
    must("relower_success_region_bit(name)", "AC1 coverage on content restamp", cas_win)
    must("relower_success_region_bit(d)", "AC1 dependent coverage", cas_win)

    store = ixx.find("void store_define_v2(")
    store_win = ixx[store : store + 2200] if store >= 0 else ""
    must("content_stored_this_epoch = true", "AC1 store sets latch", store_win)
    must("abort_map_invalid = false", "AC1 store clears abort before restamp", store_win)
    must("restamp_cache_entry_live_(entry)", "AC1 store still restamps", store_win)

    rb = ixx.find("bool relower_define_blocks(")
    rb_win = ixx[rb : rb + 22000] if rb >= 0 else ""
    must("Issue #3481", "AC2 relower cite", rb_win)
    must("ack_cache_entry_fences_live_", "AC2 peel ack", rb_win)
    ack = rb_win.find("ack_cache_entry_fences_live_")
    per_fn = rb_win.find("restamp after successful per-fn")
    if ack < 0 or per_fn < 0 or ack > per_fn:
        fails.append("AC2: instr peel ack must precede per-fn restamp")
    else:
        peel_arm = rb_win[ack:per_fn]
        if "restamp_cache_entry_live_" in peel_arm:
            fails.append("AC2: instr peel arm must not restamp_cache_entry_live_")
    must("content_stored_this_epoch", "AC2 skip-as-clean content gate", rb_win)
    must("3481 AC2: instr peel arm does not restamp content stamps", "AC2 test", t)

    fac = hur.find("hard_invalidate_via_facade(const char* name, ReemitReason reason)")
    nxt = hur.find("\nvoid HotUpdateRegistry::", fac + 1) if fac >= 0 else -1
    fac_win = hur[fac:nxt] if nxt > fac else hur[fac : fac + 8000] if fac >= 0 else ""
    must("decide_and_reemit(aura_get_aot_defuse_version(), reason)", "AC3 facade reemit stays", fac_win)
    must("Issue #3481", "AC3 facade cite", fac_win)
    must("IR cache is NOT restamped here", "AC3 no content restamp", fac_win)
    must("aura_aot_invalidate_owner_slot_for_func_id", "AC3 #3377 stays", fac_win)
    must("aura_aot_mark_peer_ir_name_soft_stale", "AC3 #3351 stays", fac_win)
    must_not("restamp_cache_entry_live_", "AC3 facade no restamp call", fac_win)
    must("3481", "AC3 mangle cite", mangle)
    must("3481 AC3: facade does not content-restamp", "AC3 test", t)

    must("aura_production_defaults_active_probe() == 0", "AC4 Soft returns false", fac_win)
    must("3481 AC4: #2183 mismatch-force-relower still fires", "AC4 test", t)
    must("cache_stamp_mismatch_force_relower_total", "AC4 2183 metric stays", t)

    fd = ixx.find("void force_ir_cache_dirty_after_abort()")
    fd_win = ixx[fd : fd + 3200] if fd >= 0 else ""
    must("source_to_ir_map.clear()", "AC5 clear map", fd_win)
    must("abort_map_invalid = true", "AC5 invalid flag", fd_win)
    must("stamp_version(0, 0, 0, 0)", "AC5 zero stamps", fd_win)
    must("content_stored_this_epoch = false", "AC5 content untrusted", fd_win)
    must("3481 AC5: abort_map_invalid", "AC5 test", t)

    must("coverage-only", "AC6 BoundaryExit", bnd)
    must("last_reemit_success_region_mask", "AC6 coverage mask", hur)
    must("Issue #3481", "AC6 pure cite", pure)
    must("check_aot_facade_content_restamp_3481", "AC6 build.py", build)
    prev = build.find("check_abort_restore_stale_map_stamp_3324")
    ours = build.find("check_aot_facade_content_restamp_3481")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #3324")
    must_not("schema-3481", "AC6 no schema-3481", ixx)
    must_not("g_3481_", "AC6 no g_3481_*", ixx)
    must_not("query:content-stored", "AC6 no new query key", ixx)
    if _read("tests/compiler/test_issue_3481.cpp") or _read("tests/issues/test_issue_3481.cpp"):
        fails.append("AC6: test_issue_3481.cpp present (forbidden #81967)")
    if _read("docs/design/3481-aot-restamp-dirty.md"):
        fails.append("AC6: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3481 aot_facade_content_restamp:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3481 aot_facade_content_restamp: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
