#!/usr/bin/env python3
"""Issue #3140: CastOp typed-meta Phase C — JIT deopt on missing/aging
typed-meta under Production only. Closes residual of #2624 Phase A.

Contract (one row per AC):
  AC1  Production + missing typed-meta OR epoch_lags(current_stamp) →
       castop_typed_meta_phase_c_deopt_total++ AND aura_jit_batch_deopt_for(fn)
  AC2  Soft path unchanged — `production_path_enabled` gate keeps Soft at
       zero cost (no extra counter bumps, no deopt branch fired)
  AC3  Quiet (meta present + epoch >= current_stamp OR current_stamp==0)
       → no deopt counter bump, no force-relower
  AC4  Additive counter only — `castop_typed_meta_phase_c_deopt_total`,
       `castop_typed_meta_phase_c_wired` flag (default 1); no permanent
       dirty bits on Quiet (no schema change to existing tables)
  AC5  Source-cite castop_typed_meta.h (lag helper + epoch field) +
       castop_density_policy.hh (gate) + lowering_impl.cpp (stamp plumbing).
       Extend test_castop_density_hard.cpp — no docs/design/, no
       tests/issues/test_issue_3140.cpp (per #81967/#1655 aura philosophy)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main(m):  # noqa: F811
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    meta = _read("src/compiler/castop_typed_meta.h")
    pol = _read("src/compiler/castop_density_policy.hh")
    low = _read("src/compiler/lowering_impl.cpp")
    test = _read("tests/compiler/test_castop_density_hard.cpp")
    build = _read("build.py")
    manifest = _read("scripts/coverage/manifests/3140.json")

    # ── AC1: Production + missing/aging meta → deopt + counter bump ─────
    must("Issue #3140 Phase C", "AC1 policy cite", pol)
    must("castop_typed_meta_phase_c_hot_entry_deopt", "AC1 gate helper", pol)
    must("castop_typed_meta_phase_c_lags", "AC1 lag helper (in meta)", meta)
    must("castop_typed_meta_phase_c_deopt_total", "AC1 counter", meta)
    must("kCastOpTypedMetaPhaseCIssue = 3140", "AC1 issue stamp", meta)
    must("castop_typed_meta_phase_c_wired", "AC1 wired flag", meta)
    must("epoch_or_mid", "AC1 epoch field in struct", meta)
    must("aura_jit_batch_deopt_for(fn_name, current_stamp)", "AC1 force-relower mechanism retained", pol)
    must("Issue #3140 Phase C", "AC1 lowering cite", low)
    must("last_type_linear_commit_proof_stamp_v_read", "AC1 current stamp read", low)
    must(
        "stamp_castop_typed_meta(site, src_type_id, dst, narrow_evidence, type_tag,",
        "AC1 6-arg stamp call in lowering",
        low,
    )
    must("AC1: missing meta", "AC1 missing → lag doc", meta)
    must("AC3: meta.epoch_or_mid >= current_stamp OR current_stamp == 0", "AC1 vs AC3 doc", meta)
    must("AC2: Soft path never calls this", "AC2 Soft doc", meta)
    must("ac3140_1_production_missing_meta_deopt", "AC1 test function", test)
    must("3140 AC1: Production + missing meta", "AC1 test marker", test)

    # ── AC2: Soft path unchanged (zero cost) ─────────────────────────────
    must("production_path_enabled", "AC2 production gate", pol)
    must("castop_typed_meta_missing_total", "AC2 Soft missing counter unchanged", meta)
    must("castop_typed_meta_missing_total.fetch_add(1", "AC2 Soft miss bump unchanged", meta)
    must("ac3140_3_soft_path_zero_cost", "AC3 test function (AC2 verify)", test)
    must("3140 AC3: Soft path", "AC2 test marker", test)

    # ── AC3: Quiet epoch-match → zero extra atomics ─────────────────────
    must("AC3: Quiet", "AC3 Quiet doc", meta)
    must("current_stamp == 0", "AC3 zero-stamp Quiet branch", meta)
    must("hit->epoch_or_mid < current_stamp", "AC3 lag comparator", meta)
    must("ac3140_4_quiet_epoch_match_zero_extra", "AC4 test function (AC3 verify)", test)
    must("3140 AC4: current_stamp==0", "AC3 test marker", test)

    # ── AC4: Additive counter only ──────────────────────────────────────
    must("set_castop_typed_meta_phase_c_wired_for_test", "AC4 test toggle", meta)
    must("reset_castop_typed_meta_phase_c_for_test", "AC4 reset helper", meta)
    must("castop_typed_meta_phase_c_deopt_total_v_read", "AC4 accessor", meta)
    must("castop_typed_meta_last_epoch_or_mid", "AC4 last-epoch track", meta)
    must("ac3140_5_additive_counter_and_source_cite", "AC5 test function (AC4 verify)", test)

    # ── AC5: source-cite + extend test_castop_density_hard.cpp + no docs ──
    must("check_castop_typed_meta_phase_c_3140.py", "AC5 build.py wires linter", build)
    if "check_castop_typed_meta_phase_c_3140.py" not in manifest:
        fails.append("AC5: manifest 3140.json missing check_castop_typed_meta_phase_c_3140.py")
    must("ac3140_5_additive_counter_and_source_cite", "AC5 test function", test)
    must("3140 AC5: no docs/design/3140-*.md", "AC5 no-design test marker", test)
    must("3140 AC5: no tests/issues/test_issue_3140.cpp", "AC5 no-test_issue_3140 test marker", test)

    if fails:
        print("FAIL: Issue #3140 Phase C linter found", len(fails), "problems:")
        for f in fails:
            print(" -", f)
        return 1
    print("OK: Issue #3140 Phase C — production JIT deopt on typed-meta missing/aging.")
    return 0


if __name__ == "__main__":
    sys.exit(main(None))
