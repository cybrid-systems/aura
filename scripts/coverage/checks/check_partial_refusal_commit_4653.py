#!/usr/bin/env python3
"""Issue #4653: every want_partial == false reason must refuse the per-fn commit.

Mechanism under test (src/compiler/service.ixx):

  CompilerService::relower_dirty_defines_from_workspace() takes the ELSE arm
  (the full re-lower arm) whenever want_partial == false.  The else arm calls

      relower_define_blocks(name, canonical, ..., commit_per_fn_partial = X)

  Before #4653, X was "!impact_refused_partial" -- i.e. only the #4422 impact
  refusal turned the per-fn commit arm off.  Every other fail-closed reason
  that clears want_partial (zero block mask #3484, unknown callee cone #3656,
  shape flip #3986, persist attribution #3618, map-prepare / storm #2190)
  left impact_refused_partial false, so the per-fn peel still ran.

  The per-fn peel (relower_define_function) only replaces the instruction
  slots whose instruction-dirty bit is set.  IRCacheEntry::mark_all_blocks_dirty
  used to set the block bits but NOT the AoS instruction mask, so the peel kept
  the OLD instruction of a same-shape edit (idf == 0 slots) and the per-fn
  success arm promoted that stale content (dirty = false,
  content_stored_this_epoch = true, live restamp) -> lookup_define_v2
  served pre-edit IR.

Contract rows (one per AC):

  AC1  The workspace peel's full arm passes a refusal-aware
       commit_per_fn_partial on the production face: the argument is not the
       bare "!impact_refused_partial" any more, and the production face
       (production_defaults_active / AuditStrategy::Full) forces the refusal.
  AC2  IRCacheEntry::mark_all_blocks_dirty cascades the AoS instruction-dirty
       mask to the live irs layout, so a wholesale dirty mark cannot leave
       clean (idf == 0) instruction slots behind.
  AC3  The new refusal arm installs no native code and stamps no
       last_reemit_success_region_mask content promotion.
  AC4  No new query key: query:incremental-relower-stats is untouched.

Exit codes:
  0 -- clean
  1 -- at least one required pattern missing OR forbidden artefact present
  2 -- invocation error

Usage:
  python3 scripts/coverage/checks/check_partial_refusal_commit_4653.py
  python3 scripts/coverage/checks/check_partial_refusal_commit_4653.py --strict
  python3 scripts/coverage/checks/check_partial_refusal_commit_4653.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SERVICE = ROOT / "src" / "compiler" / "service.ixx"
HOT_UPDATE = ROOT / "src" / "compiler" / "hot_update_registry.hh"
OBS_EVAL = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"


def _read(path: Path) -> str:
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def _body_after(text: str, anchor: str, span: int = 6000) -> str:
    pos = text.find(anchor)
    if pos == -1:
        return ""
    return text[pos : pos + span]


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #4653 partial-refusal commit contract")
    parser.add_argument("--strict", action="store_true", help="exit 1 on hit")
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    fails: list[str] = []
    rows: list[dict] = []

    svc = _read(SERVICE)
    hot = _read(HOT_UPDATE)
    obs = _read(OBS_EVAL)

    if not svc:
        print("FAIL: src/compiler/service.ixx not readable", file=sys.stderr)
        return 2

    peel = _body_after(svc, "std::size_t relower_dirty_defines_from_workspace()", 60000)
    if not peel:
        fails.append("AC1: relower_dirty_defines_from_workspace not found in service.ixx")
        peel = ""

    # -- AC1: the full arm passes a refusal-aware commit_per_fn_partial --
    # Find every relower_define_blocks(...) call inside the peel that passes
    # the commit_per_fn_partial argument, and inspect the argument.
    calls = list(
        re.finditer(
            r"relower_define_blocks\s*\(([^;]*?)\)\s*\)",
            peel,
            re.S,
        )
    )
    armed = []
    for m in calls:
        arg_text = m.group(1)
        if "commit_per_fn_partial" not in arg_text:
            continue
        arg = arg_text.split("commit_per_fn_partial")[-1]
        arg = arg.split(")")[0].strip().rstrip(",").strip()
        arg = arg.lstrip("*=/ \t\n")
        armed.append(arg)

    ac1_found = bool(armed)
    ac1_refusal_aware = any(("4653" in a) or ("production_face" in a) for a in armed)
    ac1_not_bare_impact = all(a != "!impact_refused_partial" for a in armed)
    ac1_production_face = (
        "production_face_4653" in peel and "production_defaults_active()" in peel and "AuditStrategy::Full" in peel
    )
    ac1_cite = "Issue #4653" in peel
    ac1_ok = ac1_found and ac1_refusal_aware and ac1_not_bare_impact and ac1_production_face and ac1_cite
    if not ac1_found:
        fails.append("AC1: no relower_define_blocks(...) call inside the workspace peel passes commit_per_fn_partial")
    if armed and not ac1_refusal_aware:
        fails.append(
            "AC1: the peel's commit_per_fn_partial argument is not refusal-aware: " + ", ".join(repr(a) for a in armed)
        )
    if armed and not ac1_not_bare_impact:
        fails.append(
            "AC1: the peel still passes the bare !impact_refused_partial -- only the "
            "#4422 impact refusal would refuse the per-fn commit arm"
        )
    if not ac1_production_face:
        fails.append(
            "AC1: the peel does not consult the production face "
            "(production_defaults_active / AuditStrategy::Full) for the refusal"
        )
    if not ac1_cite:
        fails.append("AC1: service.ixx does not cite Issue #4653 in the workspace peel")
    rows.append(
        {
            "ac": "AC1_refusal_aware_commit_arg",
            "ok": ac1_ok,
            "armed_call_args": armed,
            "refusal_aware": ac1_refusal_aware,
            "not_bare_impact": ac1_not_bare_impact,
            "production_face": ac1_production_face,
            "cites_4653": ac1_cite,
        }
    )

    # -- AC2: mark_all_blocks_dirty cascades the AoS instruction mask --
    mark_all = _body_after(svc, "void mark_all_blocks_dirty()", 2500)
    ac2_defined = bool(mark_all)
    ac2_cascade = "mark_all_instruction_dirty_for_current_layout_" in mark_all
    ac2_helper = "mark_all_instruction_dirty_for_current_layout_" in svc
    ac2_cite = "Issue #4653" in mark_all
    ac2_ok = ac2_defined and ac2_cascade and ac2_helper and ac2_cite
    if not ac2_defined:
        fails.append("AC2: IRCacheEntry::mark_all_blocks_dirty not found in service.ixx")
    if not ac2_cascade:
        fails.append(
            "AC2: mark_all_blocks_dirty does not cascade the AoS instruction-dirty mask "
            "(a same-shape edit would keep its stale idf == 0 slots)"
        )
    if not ac2_helper:
        fails.append(
            "AC2: the instruction-mask cascade helper mark_all_instruction_dirty_for_current_layout_ is missing"
        )
    if not ac2_cite:
        fails.append("AC2: service.ixx does not cite Issue #4653 for the instruction-mask cascade")
    rows.append(
        {
            "ac": "AC2_instruction_mask_cascade",
            "ok": ac2_ok,
            "cascade_call": ac2_cascade,
            "helper_defined": ac2_helper,
            "cites_4653": ac2_cite,
        }
    )

    # -- AC3: the refusal arm installs no native and stamps no coverage --
    refusal_win = ""
    if "commit_per_fn_partial_4653" in peel:
        pos = peel.find("commit_per_fn_partial_4653")
        refusal_win = peel[max(0, pos - 3000) : pos + 3000]
    ac3_win = bool(refusal_win)
    ac3_no_native = not any(
        tok in refusal_win
        for tok in (
            "aura_drop_jit_fn_native_for_define",
            "jit_.partial_recompile",
            "install_native",
            "ir_content_untrusted_for_native = false",
        )
    )
    ac3_no_coverage = "last_reemit_success_region_mask" not in refusal_win
    ac3_noop_stays = (
        "void note_relower_success_coverage(std::uint64_t region_bit) noexcept { (void)region_bit; }" in hot
    )
    ac3_ok = ac3_win and ac3_no_native and ac3_no_coverage and ac3_noop_stays
    if ac3_win:
        if not ac3_no_native:
            fails.append(
                "AC3: the refusal arm installs native code -- the fix must not install "
                "native while ir_content_untrusted_for_native is set"
            )
        if not ac3_no_coverage:
            fails.append("AC3: the refusal arm stamps last_reemit_success_region_mask as a content promotion")
    if not ac3_noop_stays:
        fails.append(
            "AC3: note_relower_success_coverage is no longer a no-op -- "
            "last_reemit_success_region_mask must not become a content promotion"
        )
    rows.append(
        {
            "ac": "AC3_no_native_no_coverage_stamp",
            "ok": ac3_ok,
            "refusal_window_found": ac3_win,
            "no_native_install": ac3_no_native,
            "no_coverage_stamp": ac3_no_coverage,
            "coverage_hook_stays_noop": ac3_noop_stays,
        }
    )

    # -- AC4: no new query key on query:incremental-relower-stats --
    ac4_schema = "query:incremental-relower-stats" in obs
    ac4_no_new_key = "4653" not in obs
    ac4_ok = ac4_schema and ac4_no_new_key
    if not ac4_schema:
        fails.append("AC4: query:incremental-relower-stats schema not found in evaluator_primitives_obs_eval.cpp")
    if not ac4_no_new_key:
        fails.append("AC4: a new query key for #4653 was added -- the stats schema must not change")
    rows.append(
        {
            "ac": "AC4_no_new_query_key",
            "ok": ac4_ok,
            "schema_present": ac4_schema,
            "no_4653_key": ac4_no_new_key,
        }
    )

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
        "\nOK: Issue #4653 -- every want_partial == false reason refuses the "
        "per-fn commit on the production face, and a wholesale dirty mark "
        "cascades the instruction mask (no stale idf == 0 slots)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
