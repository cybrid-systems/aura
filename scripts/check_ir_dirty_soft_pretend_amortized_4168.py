#!/usr/bin/env python3
"""Issue #4168: Soft dirty/batch-only misconfig must not pretend multi-round
amortization — dashboard honesty face (pretend_amortized + HP readiness refuse).

I6 requires continuous mutate amortized in the dirty cone under Production.
Soft/Off paths skip permanent dirty-bit writes (observe-only cone branches in
dirty_propagation.ixx gate on production_defaults_active) and env=0 can force
AURA_IR_DIRTY_BATCH_ONLY=0 (#3201), so Soft counters read "green" (residual
multi-via-single == 0, #2936) while incremental lower/opt may re-scan whole
modules. Fix contract: the query/dashboard surfaces pretend-amortized=1 and
refuses HP readiness claims whenever the production face is inactive; Soft
observe counters are kept; Production batch APIs are unchanged.

Gate rows:
  G1  ir_soa.ixx exports ir_dirty_soft_pretend_amortized +
      ir_dirty_hp_amortization_ready reusing the single
      aura_production_defaults_active_probe (#3201 model; no second model).
  G2  probe-missing → pretend stays true (Soft binary honest); HP readiness
      granted only when the production face is active.
  G3  Soft observe counters preserved (#2774 residual cascades/marks atoms
      still exported); ir_dirty_batch_only_hard env contract intact
      (env=0 / env=1 / unset→probe); no Production batch API removal.
  G4  query:soa-dirty-stats (evaluator_primitives_obs_jit.cpp) surfaces
      schema-4168 + issue-4168 + ir-dirty-pretend-amortized +
      ir-dirty-hp-amortization-ready + ir-dirty-honesty-wired.
  G5  test ACs in test_batch_dirty_discipline (#81967): ac4168_1..ac4168_4
      present and dispatched in run_test_batch_dirty_discipline.
  G6  no test_issue_4168.cpp; no docs/design/4168-* (#1655/#81967).
  G7  build.py wires this linter; scripts/coverage/root_check_allowlist.txt
      carries the filename.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

failures: list[str] = []


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def main() -> int:
    soa = _read("src/compiler/ir_soa.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_batch_dirty_discipline.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    print("Issue #4168 — Soft dirty/batch-only honesty (pretend amortized)")
    # G1: exported honesty probes reusing the single C probe.
    must("kIrSoaSoftPretendAmortizedIssue = 4168" in soa, "G1: issue stamp")
    must(
        "ir_dirty_soft_pretend_amortized" in soa and "ir_dirty_hp_amortization_ready" in soa,
        "G1: honesty probes exported",
    )
    must(
        len(re.findall(r"aura_production_defaults_active_probe", soa)) >= 3,
        "G1: reuses aura_production_defaults_active_probe (no second model)",
    )
    # G2: probe-missing → pretend true; HP ready = production face only.
    must(
        re.search(
            r"ir_dirty_soft_pretend_amortized\(\) noexcept \{[^}]*probe == nullptr[^}]*return true;",
            soa,
            re.DOTALL,
        )
        is not None,
        "G2: probe-missing → pretend stays true",
    )
    must(
        re.search(
            r"ir_dirty_hp_amortization_ready\(\) noexcept \{\s*"
            r"return !ir_dirty_soft_pretend_amortized\(\);",
            soa,
        )
        is not None,
        "G2: HP claim granted only under production face",
    )
    # G3: Soft observe counters + env contract + batch APIs intact.
    must(
        "g_ir_soa_residual_multi_via_single_cascades_total" in soa
        and "g_ir_soa_residual_multi_via_single_marks_total" in soa,
        "G3: Soft observe counters preserved",
    )
    must(
        re.search(
            r"ir_dirty_batch_only_hard\(\) noexcept \{.*?e\[0\] == '0'.*?e\[0\] == '1'",
            soa,
            re.DOTALL,
        )
        is not None,
        "G3: ir_dirty_batch_only_hard env contract intact",
    )
    must(
        "mark_blocks_dirty" in soa and "mark_blocks_dirty_bits_only" in soa and "mark_all_blocks_dirty" in soa,
        "G3: Production batch APIs unchanged",
    )
    # G4: dashboard keys on query:soa-dirty-stats.
    for key in (
        '"schema-4168"',
        '"issue-4168"',
        '"ir-dirty-pretend-amortized"',
        '"ir-dirty-hp-amortization-ready"',
        '"ir-dirty-honesty-wired"',
    ):
        must(key in obs, f"G4: dashboard key {key}")
    # G5: ACs present and dispatched.
    ac4168_fns = (
        "ac4168_1_soft_pretends",
        "ac4168_2_residual_zero_not_hp_proof",
        "ac4168_3_production_face_grants",
        "ac4168_4_source_and_linter",
    )
    for fn in ac4168_fns:
        must(fn in test, f"G5: {fn}")
    runner = test.split("int run_test_batch_dirty_discipline()", 1)[-1]
    must(
        all(f"{fn}();" in runner for fn in ac4168_fns),
        "G5: ACs dispatched in run_test_batch_dirty_discipline",
    )
    # G6: no invent.
    must(
        not (ROOT / "tests/compiler/test_issue_4168.cpp").is_file(),
        "G6: no test_issue_4168.cpp",
    )
    must(not list((ROOT / "docs/design").glob("4168-*")), "G6: no docs/design/4168-*")
    # G7: build.py + allowlist.
    must("check_ir_dirty_soft_pretend_amortized_4168" in build, "G7: build.py wires linter")
    must(
        "check_ir_dirty_soft_pretend_amortized_4168.py" in allow,
        "G7: allowlist row",
    )

    if failures:
        print(f"\n{len(failures)} FAIL")
        return 1
    print("\nAll rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
