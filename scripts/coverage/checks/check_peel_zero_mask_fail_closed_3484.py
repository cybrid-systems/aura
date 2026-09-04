#!/usr/bin/env python3
"""Issue #3484: peel must not count dirty_n==0 / instr-peel skip as success.

#1495 workspace peel ++ok on persist-empty zero mask. #2133 instr peel
returns true after pass-only (no AST rewrite). Combined with the
production depth-1/#3381 cone, a name that entered dirty_names can look
successfully peeled while IR is still pre-mutate.

Production / Full: zero-mask cone name fail-closed full (reuse
partial_forced_full_by_impact_total). Instr peel without AST rewrite
falls through to per-fn / store_define_v2. Soft / Off keep ++ok skip
and return-true peel. No new query keys.

Contract:
  AC1  production dirty_n==0 does not ++ok skip; mark_all_blocks_dirty
  AC2  production instr peel acks then fall-through (Soft return true)
  AC3  Soft ++ok + should_partial_relower dirty_count==0 unchanged
  AC4  no new query key; soak uses existing impact / should_relower
  AC5  extend test_cascade_relower_silent_skip (AC3 unwired-hook stays)
  AC6  linter AFTER #3474; no invent / docs/design

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
    pure = _read("src/compiler/ir_cache_pure.ixx")
    test = _read("tests/compiler/test_cascade_relower_silent_skip.cpp")
    build = _read("build.py")

    rel_pos = ixx.find("std::size_t relower_dirty_defines_from_workspace()")
    peel = ixx[rel_pos : rel_pos + 20000] if rel_pos >= 0 else ""
    must("Issue #3484", "AC1 peel cite", peel)
    must("zero_mask_forced_full", "AC1 flag", peel)
    must("mark_all_blocks_dirty()", "AC1 fail-closed mark", peel)
    must("partial_forced_full_by_impact_total", "AC1 reuse counter", peel)
    must("content_stored_this_epoch = false", "AC1 content untrusted", peel)
    # Soft ++ok skip is retained; production wraps it so ++ok is not the
    # zero-mask success path under production_defaults_active.
    ok_pos = peel.find("++ok;")
    if ok_pos < 0:
        fails.append("AC3: Soft ++ok skip missing")
    else:
        pre = peel[max(0, ok_pos - 900) : ok_pos]
        must("production_defaults_active()", "AC1 ++ok behind production gate", pre)
        must("if (!production_zero)", "AC1 production does not ++ok skip", pre)

    rb = ixx.find("bool relower_define_blocks(")
    rb_win = ixx[rb : rb + 22000] if rb >= 0 else ""
    must("Issue #3484", "AC2 relower cite", rb_win)
    must("ack_cache_entry_fences_live_", "AC2 peel ack stays", rb_win)
    ack = rb_win.find("ack_cache_entry_fences_live_")
    per_fn = rb_win.find("restamp after successful per-fn")
    if ack < 0 or per_fn < 0 or ack > per_fn:
        fails.append("AC2: instr peel ack must precede per-fn restamp")
    else:
        peel_arm = rb_win[ack:per_fn]
        must("production_defaults_active()", "AC2 production gate", peel_arm)
        must("mark_all_blocks_dirty()", "AC2 production re-dirty", peel_arm)
        must("partial_forced_full_by_impact_total", "AC2 reuse counter", peel_arm)
        must("return true", "AC3 Soft return true", peel_arm)
        must_not("restamp_cache_entry_live_", "AC2 no content restamp", peel_arm)

    must("if (dirty_count == 0)", "AC3 should_partial_relower", pure)
    must("should_partial_relower", "AC3 helper", pure)

    must("cascade_relower_skipped_total", "AC5 AC3 unwired-hook stays", test)
    must("3484 AC5: production zero-mask caller soak", "AC5 plant soak", test)
    must("3484 AC5: IR rewritten or lookup==1 (not silent skip)", "AC5 lookup", test)
    must("plant_zero_mask_caller_for_test", "AC5 plant helper", test)
    must("partial_forced_full_by_impact_total", "AC4 soak metric", test)
    must("should_relower_total", "AC4 soak should_relower", test)
    must("3484 AC3: Soft clean peel does not force-full", "AC3 Soft soak", test)

    must("check_peel_zero_mask_fail_closed_3484", "AC6 build.py", build)
    prev = build.find("check_production_called_by_cone_bfs_3474")
    ours = build.find("check_peel_zero_mask_fail_closed_3484")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #3474")

    must("plant_zero_mask_caller_for_test", "AC5 helper", ixx)
    must_not("schema-3484", "AC4 no schema-3484", ixx)
    must_not("g_3484_", "AC4 no g_3484_*", ixx)
    must_not("query:peel-zero-mask", "AC4 no new query key", ixx)
    if (ROOT / "tests" / "compiler" / "test_issue_3484.cpp").is_file():
        fails.append("AC6: test_issue_3484.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3484.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3484.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3484-*")):
            fails.append(f"AC6: docs/design/{f.name} present")

    if fails:
        print("FAIL #3484 peel_zero_mask_fail_closed:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3484 peel_zero_mask_fail_closed: production fail-closed; Soft skip kept")
    return 0


if __name__ == "__main__":
    sys.exit(main())
