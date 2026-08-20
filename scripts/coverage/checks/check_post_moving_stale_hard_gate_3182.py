#!/usr/bin/env python3
"""Issue #3182: post-Moving stale canary → AdaptiveCompactResult hard gate.

Refines #3055 / #3092. Per-arena observe axis (LiveCompactResult.
post_moving_stale_count) is already covered by ac3055_x. #3182 adds the
AGGREGATE field + AGGREGATE hard gate at the AdaptiveCompactResult
level so the unified Phase-5 pin_contract_held surfaces post-Moving
EnvFrame / Closure / FFI / JIT residual even if a future path
populates per-arena stale without folding it into r.pin_contract_held.

Contract (one row per AC):
  AC1  AdaptiveCompactResult.post_moving_stale_count_total field
       default-initialized + cite #3182 + inside the right struct
  AC2  compact_all_moving_pinned() aggregates r.post_moving_stale_count
       into out.post_moving_stale_count_total + cite #3182
  AC3  Hard gate: objects_moved_total > 0 AND
       post_moving_stale_count_total > 0 → out.pin_contract_held = false
       + cite #3182 + comment references AC2 (unified gate)
  AC4  AdaptiveCompactResult::empty() checks
       post_moving_stale_count_total == 0
  AC5  Soft path no extra walk (AC3 of #3182): hard gate short-circuits
       on objects_moved_total == 0; Phase-5 only calls
       compact_all_moving_pinned() (Moving path).
  AC6  EnvFrameLifetimeGuard protocol unchanged: scan_skip_freed,
       hold_generation, hold_gen_at_enter preserved; no second pin
       registry introduced.
  AC7  tests/core/test_moving_densify_fail_closed.cpp extended with
       ac3182_1-7 (extends existing src/-aligned suite per #81934; no
       tests/issues/test_issue_3182.cpp; no docs/design/3182-* per #1655)
  AC8  this linter wired in build.py; no docs/design/3182-* (per #1655)

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_post_moving_stale_hard_gate_3182.py            # report
  python3 scripts/coverage/checks/check_post_moving_stale_hard_gate_3182.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_post_moving_stale_hard_gate_3182.py --json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

ARENA_IXX = ROOT / "src" / "core" / "arena.ixx"
EV_MUTATION_BOUNDARY = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
ENVFRAME_LIFETIME = ROOT / "src" / "core" / "envframe_lifetime.ixx"
TEST_FILE = ROOT / "tests" / "core" / "test_moving_densify_fail_closed.cpp"
BUILD_PY = ROOT / "build.py"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_DIR = ROOT / "tests" / "issues" / "test_issue_3182.cpp"


def _read(rel: Path) -> str:
    if not rel.is_file():
        return ""
    return rel.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #3182 post-Moving stale hard gate linter")
    parser.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    fails: list[str] = []
    rows: list[dict] = []

    arena = _read(ARENA_IXX)
    ev = _read(EV_MUTATION_BOUNDARY)
    envframe = _read(ENVFRAME_LIFETIME)
    test = _read(TEST_FILE)
    build = _read(BUILD_PY)

    # ── AC1: AdaptiveCompactResult.post_moving_stale_count_total field ──
    ac1_field = arena.find("post_moving_stale_count_total = 0;") != -1
    struct_pos = arena.find("export struct AdaptiveCompactResult {")
    struct_end = struct_pos != -1 and arena.find("};", struct_pos) or -1
    if struct_pos != -1 and struct_end != -1:
        body = arena[struct_pos:struct_end]
        ac1_in_struct = "post_moving_stale_count_total" in body
        ac1_cites = "Issue #3182" in body
    else:
        ac1_in_struct = False
        ac1_cites = False
    ac1_ok = ac1_field and ac1_in_struct and ac1_cites
    if not ac1_field:
        fails.append("AC1: aggregate field default-init missing")
    if not ac1_in_struct:
        fails.append("AC1: aggregate field not in AdaptiveCompactResult struct")
    if not ac1_cites:
        fails.append("AC1: aggregate field comment missing Issue #3182 cite")
    rows.append(
        {
            "ac": "AC1_aggregate_field",
            "ok": ac1_ok,
            "default_init": ac1_field,
            "in_struct": ac1_in_struct,
            "cites_3182": ac1_cites,
        }
    )

    # ── AC2: compact_all_moving_pinned aggregation ──
    fn_pos = arena.find("compact_all_moving_pinned() noexcept")
    fn_end = fn_pos != -1 and arena.find("return out;", fn_pos) or -1
    if fn_pos != -1 and fn_end != -1:
        fn_window = arena[fn_pos:fn_end]
        ac2_aggregate = "out.post_moving_stale_count_total += r.post_moving_stale_count;" in fn_window
        ac2_cites = "Issue #3182" in fn_window
    else:
        ac2_aggregate = False
        ac2_cites = False
    ac2_ok = ac2_aggregate and ac2_cites
    if not ac2_aggregate:
        fails.append("AC2: compact_all_moving_pinned missing post_moving_stale_count_total aggregation")
    if not ac2_cites:
        fails.append("AC2: aggregation loop missing Issue #3182 cite")
    rows.append({"ac": "AC2_aggregation", "ok": ac2_ok, "aggregate": ac2_aggregate, "cites_3182": ac2_cites})

    # ── AC3: hard gate condition ──
    gate_pos = arena.find("if (out.objects_moved_total > 0 && out.post_moving_stale_count_total > 0)")
    if gate_pos != -1:
        # Window extends BEFORE the `if` to include the comment that
        # precedes it (comment is above the `if` line).
        win = arena[max(0, gate_pos - 400) : gate_pos + 300]
        ac3_sets_false = "out.pin_contract_held = false" in win
        ac3_cites = "Issue #3182" in win
        ac3_ac2_ref = "AC2" in win
    else:
        ac3_sets_false = False
        ac3_cites = False
        ac3_ac2_ref = False
    ac3_ok = gate_pos != -1 and ac3_sets_false and ac3_cites and ac3_ac2_ref
    if gate_pos == -1:
        fails.append("AC3: hard-gate condition missing")
    if not ac3_sets_false:
        fails.append("AC3: hard gate must set pin_contract_held=false")
    if not ac3_cites:
        fails.append("AC3: hard gate comment missing Issue #3182 cite")
    if not ac3_ac2_ref:
        fails.append("AC3: hard gate comment must reference AC2 (unified gate)")
    rows.append(
        {
            "ac": "AC3_hard_gate",
            "ok": ac3_ok,
            "condition_present": gate_pos != -1,
            "sets_false": ac3_sets_false,
            "cites_3182": ac3_cites,
            "ac2_ref": ac3_ac2_ref,
        }
    )

    # ── AC4: AdaptiveCompactResult::empty() ──
    # empty() is defined inline in the struct (no `AdaptiveCompactResult::` prefix).
    # Search AFTER `export struct AdaptiveCompactResult {` to disambiguate from
    # LiveCompactResult::empty() (which appears earlier in the file).
    acr_pos = arena.find("export struct AdaptiveCompactResult {")
    empty_pos = -1
    if acr_pos != -1:
        empty_pos = arena.find("bool empty() const noexcept", acr_pos)
    if empty_pos != -1:
        win = arena[empty_pos : empty_pos + 500]
        ac4_checks_field = "post_moving_stale_count_total == 0" in win
    else:
        ac4_checks_field = False
    ac4_ok = empty_pos != -1 and ac4_checks_field
    if empty_pos == -1:
        fails.append("AC4: AdaptiveCompactResult empty() not found")
    if not ac4_checks_field:
        fails.append("AC4: empty() must check post_moving_stale_count_total == 0")
    rows.append(
        {"ac": "AC4_empty_considers", "ok": ac4_ok, "empty_found": empty_pos != -1, "checks_field": ac4_checks_field}
    )

    # ── AC5: Soft path no extra walk ──
    # register_known_moving_densify_root_slots lives in evaluator_mutation_boundary.cpp
    # (not arena.ixx). Phase-5 calls it + compact_all_moving_pinned (Moving path only).
    ac5_helper = "register_known_moving_densify_root_slots" in ev
    ac5_gate = gate_pos != -1
    ac5_phase5_calls = "compact_all_moving_pinned()" in ev
    ac5_ok = ac5_helper and ac5_gate and ac5_phase5_calls
    if not ac5_helper:
        fails.append("AC5: known-root registration helper missing in evaluator_mutation_boundary.cpp")
    if not ac5_gate:
        fails.append("AC5: hard gate must be present (short-circuits on objects_moved_total == 0)")
    if not ac5_phase5_calls:
        fails.append("AC5: Phase 5 must call compact_all_moving_pinned (Moving path only)")
    rows.append(
        {
            "ac": "AC5_soft_no_extra_walk",
            "ok": ac5_ok,
            "helper": ac5_helper,
            "gate": ac5_gate,
            "phase5_calls_compact": ac5_phase5_calls,
        }
    )

    # ── AC6: EnvFrameLifetimeGuard protocol unchanged ──
    ac6_scan = "scan_skip_freed" in envframe
    ac6_hold = "hold_generation" in envframe
    ac6_enter = "hold_gen_at_enter_" in envframe
    ac6_arena_cites = "Issue #3182" in arena
    ac6_no_second = "second pin registry" not in arena and "second registry" not in arena
    ac6_ok = ac6_scan and ac6_hold and ac6_enter and ac6_arena_cites and ac6_no_second
    if not ac6_scan:
        fails.append("AC6: scan_skip_freed callback must be preserved")
    if not ac6_hold:
        fails.append("AC6: hold_generation callback must be preserved")
    if not ac6_enter:
        fails.append("AC6: hold_gen_at_enter_ must be preserved")
    if not ac6_arena_cites:
        fails.append("AC6: arena.ixx must cite Issue #3182")
    if not ac6_no_second:
        fails.append("AC6: must not introduce second pin registry")
    rows.append(
        {
            "ac": "AC6_envframe_protocol_unchanged",
            "ok": ac6_ok,
            "scan_skip_freed": ac6_scan,
            "hold_generation": ac6_hold,
            "hold_gen_at_enter_": ac6_enter,
            "arena_cites_3182": ac6_arena_cites,
            "no_second_registry": ac6_no_second,
        }
    )

    # ── AC7: test extension ──
    ac7_ac1 = "ac3182_1_aggregate_field" in test
    ac7_ac3 = "ac3182_3_hard_gate" in test
    ac7_ac7 = "ac3182_7_no_invent" in test
    ac7_calls_in_run = "ac3182_1_aggregate_field();" in test
    ac7_ok = ac7_ac1 and ac7_ac3 and ac7_ac7 and ac7_calls_in_run
    if not ac7_ac1:
        fails.append("AC7: test file missing ac3182_1_aggregate_field")
    if not ac7_ac3:
        fails.append("AC7: test file missing ac3182_3_hard_gate")
    if not ac7_ac7:
        fails.append("AC7: test file missing ac3182_7_no_invent")
    if not ac7_calls_in_run:
        fails.append("AC7: test file missing call to ac3182_1_aggregate_field() in run_test")
    rows.append(
        {
            "ac": "AC7_test_extension",
            "ok": ac7_ok,
            "ac1_defined": ac7_ac1,
            "ac3_defined": ac7_ac3,
            "ac7_defined": ac7_ac7,
            "called_in_run": ac7_calls_in_run,
        }
    )

    # ── AC8: linter wired, no docs/design/, no tests/issues/ ──
    ac8_wired = "check_post_moving_stale_hard_gate_3182" in build
    no_design = True
    if DOCS_DESIGN_DIR.is_dir():
        for f in sorted(DOCS_DESIGN_DIR.glob("3182-*")):
            no_design = False
            fails.append(f"AC8: docs/design/{f.name} present (forbidden per #1655)")
    no_issues_test = not ISSUES_TEST_DIR.is_file()
    if ISSUES_TEST_DIR.is_file():
        fails.append("AC8: tests/issues/test_issue_3182.cpp present (forbidden per #81934)")
    ac8_ok = ac8_wired and no_design and no_issues_test
    if not ac8_wired:
        fails.append("AC8: linter not wired in build.py")
    rows.append(
        {
            "ac": "AC8_no_invent",
            "ok": ac8_ok,
            "linter_wired": ac8_wired,
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
    print("\nOK: Issue #3182 post-Moving stale canary → AdaptiveCompactResult hard gate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
