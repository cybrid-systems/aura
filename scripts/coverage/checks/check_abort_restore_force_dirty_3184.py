#!/usr/bin/env python3
"""Issue #3184: abort dual-topology restore must restamp/clear cache+map.

The fix was designed + wired by Issue #3033 / #3117 (dual-topology abort
hooks). #3184 verifies the contract via source-cite: every abort
restore path that calls abort_restore_dual_topology MUST also wire the
abort_ir_cache_begin_force_fn (publish live gen BEFORE restore) and
abort_ir_cache_force_dirty_fn (zero-restamp + clear source_to_ir_map
AFTER restore) callbacks.

force_ir_cache_dirty_after_abort (service.ixx L5303) implements the
required work:
  - For each ir_cache_v2_ entry:
    - entry.dirty = true
    - entry.mark_all_blocks_dirty()
    - entry.stamp_version(0, 0, 0, 0)         # zero stamps
    - entry.version_stamp_.abort_force_generation = gen  # ack live gen
    - finish_cascade_soa_dirty_sync_(entry)
    - entry.source_to_ir_map.clear()          # clear (do not rebuild)
    - entry.abort_map_invalid = true          # refuse lazy refill

Contract (one row per AC):
  AC1  CacheEntryVersionStamp carries abort_force_generation (#3069)
  AC2  should_relower treats stamp.abort_force_generation < live as
      kRelowerAbortForce force-relower
  AC3  All 3 abort_restore_dual_topology call sites in
      evaluator_mutation_boundary.cpp wire both
      abort_ir_cache_begin_force_fn (before) and
      abort_ir_cache_force_dirty_fn (after)
  AC4  force_ir_cache_dirty_after_abort (service.ixx) does the
      required force-dirty + clear source_to_ir_map work
  AC5  The begin_force + force_dirty callbacks are registered in
      service.ixx to the right service methods
  AC6  No abort_restore_dual_topology call exists without the callback
      pair (no orphan site)
  AC7  tests/ + linter wired; no docs/design/3184-* (#1655)

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_abort_restore_force_dirty_3184.py            # report
  python3 scripts/coverage/checks/check_abort_restore_force_dirty_3184.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_abort_restore_force_dirty_3184.py --json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

IR_CACHE_PURE = ROOT / "src" / "compiler" / "ir_cache_pure.ixx"
EVALUATOR = ROOT / "src" / "compiler" / "evaluator.ixx"
SERVICE = ROOT / "src" / "compiler" / "service.ixx"
MUTATION_BOUNDARY = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
BUILD_PY = ROOT / "build.py"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_DIR = ROOT / "tests" / "issues" / "test_issue_3184.cpp"


def _read(rel: Path) -> str:
    if not rel.is_file():
        return ""
    return rel.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #3184 abort dual-topology restore contract")
    parser.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    fails: list[str] = []
    rows: list[dict] = []

    ir = _read(IR_CACHE_PURE)
    _read(EVALUATOR)
    svc = _read(SERVICE)
    emb = _read(MUTATION_BOUNDARY)
    build = _read(BUILD_PY)

    # ── AC1: CacheEntryVersionStamp carries abort_force_generation ──
    ac1_struct = "struct CacheEntryVersionStamp" in ir
    ac1_field = "std::uint64_t abort_force_generation = 0;" in ir
    ac1_cites = "Issue #3069" in ir
    ac1_ok = ac1_struct and ac1_field and ac1_cites
    if not ac1_struct:
        fails.append("AC1: CacheEntryVersionStamp struct missing")
    if not ac1_field:
        fails.append("AC1: abort_force_generation field missing")
    if not ac1_cites:
        fails.append("AC1: cite #3069 missing")
    rows.append(
        {
            "ac": "AC1_cache_entry_version_stamp",
            "ok": ac1_ok,
            "struct": ac1_struct,
            "field": ac1_field,
            "cites_3069": ac1_cites,
        }
    )

    # ── AC2: should_relower treats stamp.abort_force_generation < live as force-relower ──
    ac2_should_relower = "should_relower" in ir
    ac2_check = "stamp.abort_force_generation < current_abort_force_generation" in ir
    ac2_bitflag = "kRelowerAbortForce" in ir
    ac2_ok = ac2_should_relower and ac2_check and ac2_bitflag
    if not ac2_should_relower:
        fails.append("AC2: should_relower function missing")
    if not ac2_check:
        fails.append("AC2: stamp.abort_force_generation < current_abort_force_generation check missing")
    if not ac2_bitflag:
        fails.append("AC2: kRelowerAbortForce bitflag missing")
    rows.append(
        {
            "ac": "AC2_should_relower_abort_force",
            "ok": ac2_ok,
            "should_relower": ac2_should_relower,
            "check": ac2_check,
            "bitflag": ac2_bitflag,
        }
    )

    # ── AC3: All 3 abort_restore_dual_topology call sites wire both callbacks ──
    # Find all "abort_restore_dual_topology(" call sites in emb (only code calls).
    call_sites: list[int] = []
    pos = 0
    while True:
        nxt = emb.find("abort_restore_dual_topology(", pos)
        if nxt == -1:
            break
        call_sites.append(nxt)
        pos = nxt + 1

    # For each call site, find the nearest begin_force + force_dirty before/after.
    # Expected pattern: begin_force BEFORE, restore, force_dirty AFTER (window 8000
    # chars to cover site 1's ~3570-char gap between restore and force_dirty
    # bookkeeping chain — strict sandbox site has more rollback metric writes
    # between restore and force_dirty).
    sites_ok = []
    for cs in call_sites:
        before = emb[max(0, cs - 8000) : cs]
        after = emb[cs : cs + 8000]
        begin_in_before = "abort_ir_cache_begin_force_fn_" in before
        dirty_in_after = "abort_ir_cache_force_dirty_fn_" in after
        sites_ok.append(begin_in_before and dirty_in_after)
    ac3_count = len(call_sites)
    ac3_ok = ac3_count >= 3 and all(sites_ok)
    if ac3_count < 3:
        fails.append(f"AC3: only {ac3_count} abort_restore_dual_topology call sites found (expected ≥ 3)")
    for i, ok in enumerate(sites_ok):
        if not ok:
            fails.append(
                f"AC3: abort_restore_dual_topology site {i + 1} missing begin_force (before) or force_dirty (after) callback"
            )
    rows.append(
        {
            "ac": "AC3_call_sites_wired",
            "ok": ac3_ok,
            "site_count": ac3_count,
            "sites_all_wired": all(sites_ok) if sites_ok else False,
        }
    )

    # ── AC4: force_ir_cache_dirty_after_abort does the required work ──
    ac4_func = "void force_ir_cache_dirty_after_abort()" in svc
    # The implementation must: dirty=true + mark_all_blocks_dirty + zero stamps + abort_force_generation + SoA sync + source_to_ir_map.clear + abort_map_invalid=true
    func_pos = svc.find("void force_ir_cache_dirty_after_abort()")
    if func_pos != -1:
        func_end = svc.find("\n    }\n", func_pos)
        func_end = svc.find("\n    }\n", func_end + 1) if func_end != -1 else -1
        body = svc[func_pos:func_end] if func_end != -1 else ""
    else:
        body = ""
    ac4_dirty = "entry.dirty = true" in body
    ac4_mark_blocks = "mark_all_blocks_dirty()" in body
    ac4_zero_stamps = "stamp_version(0, 0, 0, 0)" in body
    ac4_abort_gen = "version_stamp_.abort_force_generation = gen" in body
    ac4_clear_map = "source_to_ir_map.clear()" in body
    ac4_map_invalid = "abort_map_invalid = true" in body
    ac4_cites_3117 = "Issue #3117" in body
    ac4_ok = (
        ac4_func
        and ac4_dirty
        and ac4_mark_blocks
        and ac4_zero_stamps
        and ac4_abort_gen
        and ac4_clear_map
        and ac4_map_invalid
        and ac4_cites_3117
    )
    if not ac4_func:
        fails.append("AC4: force_ir_cache_dirty_after_abort function definition missing in service.ixx")
    if not ac4_dirty:
        fails.append("AC4: must set entry.dirty = true")
    if not ac4_mark_blocks:
        fails.append("AC4: must call entry.mark_all_blocks_dirty()")
    if not ac4_zero_stamps:
        fails.append("AC4: must zero stamps via entry.stamp_version(0,0,0,0)")
    if not ac4_abort_gen:
        fails.append("AC4: must set entry.version_stamp_.abort_force_generation = gen")
    if not ac4_clear_map:
        fails.append("AC4: must clear entry.source_to_ir_map")
    if not ac4_map_invalid:
        fails.append("AC4: must set entry.abort_map_invalid = true")
    if not ac4_cites_3117:
        fails.append("AC4: must cite Issue #3117 in force_ir_cache_dirty_after_abort")
    rows.append(
        {
            "ac": "AC4_force_ir_cache_dirty_after_abort",
            "ok": ac4_ok,
            "func_defined": ac4_func,
            "dirty": ac4_dirty,
            "mark_blocks": ac4_mark_blocks,
            "zero_stamps": ac4_zero_stamps,
            "abort_gen": ac4_abort_gen,
            "clear_map": ac4_clear_map,
            "map_invalid": ac4_map_invalid,
            "cites_3117": ac4_cites_3117,
        }
    )

    # ── AC5: Callbacks registered in service.ixx ──
    ac5_begin = "set_abort_ir_cache_begin_force_fn(" in svc and "begin_abort_ir_cache_force_fence()" in svc
    ac5_dirty = "set_abort_ir_cache_force_dirty_fn(" in svc and "force_ir_cache_dirty_after_abort()" in svc
    ac5_cites_3033 = "Issue #3033" in svc
    ac5_ok = ac5_begin and ac5_dirty and ac5_cites_3033
    if not ac5_begin:
        fails.append("AC5: set_abort_ir_cache_begin_force_fn registration missing or wrong target")
    if not ac5_dirty:
        fails.append("AC5: set_abort_ir_cache_force_dirty_fn registration missing or wrong target")
    if not ac5_cites_3033:
        fails.append("AC5: cite Issue #3033 missing")
    rows.append(
        {
            "ac": "AC5_callbacks_registered",
            "ok": ac5_ok,
            "begin_force_reg": ac5_begin,
            "force_dirty_reg": ac5_dirty,
            "cites_3033": ac5_cites_3033,
        }
    )

    # ── AC6: No orphan abort_restore_dual_topology site (callback pair required) ──
    # (Already covered by AC3 — each call site has the callback pair.)
    ac6_ok = ac3_ok
    rows.append({"ac": "AC6_no_orphan_site", "ok": ac6_ok, "delegated_to": "AC3"})

    # ── AC7: linter wired, no docs/design/, no tests/issues/ ──
    ac7_wired = "check_abort_restore_force_dirty_3184" in build
    no_design = True
    if DOCS_DESIGN_DIR.is_dir():
        for f in sorted(DOCS_DESIGN_DIR.glob("3184-*")):
            no_design = False
            fails.append(f"AC7: docs/design/{f.name} present (forbidden per #1655)")
    no_issues_test = not ISSUES_TEST_DIR.is_file()
    if ISSUES_TEST_DIR.is_file():
        fails.append("AC7: tests/issues/test_issue_3184.cpp present (forbidden per #81934)")
    ac7_ok = ac7_wired and no_design and no_issues_test
    if not ac7_wired:
        fails.append("AC7: linter not wired in build.py")
    rows.append(
        {
            "ac": "AC7_no_invent",
            "ok": ac7_ok,
            "linter_wired": ac7_wired,
            "no_design_docs": no_design,
            "no_issue_test": no_issues_test,
        }
    )

    # ── Report ──
    if args.json:
        out = {"ok": len(fails) == 0, "rows": rows, "fails": fails}
        print(json.dumps(out, indent=2))
        return 0 if (len(fails) == 0 or not args.strict) else 1

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1

    for r in rows:
        print(f"OK  {r['ac']}")
    print(
        "\nOK: Issue #3184 abort dual-topology restore — force-dirty + clear source_to_ir_map (verify #3033 / #3117 infrastructure)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
