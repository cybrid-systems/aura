#!/usr/bin/env python3
"""Issue #3445: pipeline last_reemit_success is reason-group coverage.

#3413 closed the decide_and_reemit `covered = demoted` fallback.
on_reemit_pipeline_call still wrote a different domain into the same
word: `candidates` (COUNT of names) ∩ emit_region_mask, then full
demoted if that was 0. 3 & emit_mask can set reason bit 0 by accident;
covered==0 stamped every demoted group. only_covered / residual_force
then consume that mixed word.

Contract:
  AC1 pipeline stamps override-only; no count∩emit; no demoted fallback
  AC2 only_covered re-promote still clears only last_success bits
  AC3 residual != 0 still drives min-dirty; playbook hint non-Idle
  AC4 relower hashed-name bits stay on the define side set
  AC5 Soft / Off / idle force mask: zero extra stores (`demoted != 0`)
  AC6 non-duplicative vs closed #3413 / #2895 / #2949 / #2978 / #3026 /
      #3136 / #3229; no new query key; no test_issue_3445.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    hot = (ROOT / "src" / "compiler" / "hot_update_registry.cpp").read_text()
    hh = (ROOT / "src" / "compiler" / "hot_update_registry.hh").read_text()
    test = (ROOT / "tests" / "compiler" / "test_aot_incremental_reemit.cpp").read_text()
    build = (ROOT / "build.py").read_text()
    mutate = (ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp").read_text()

    must("Issue #3445", "AC1 marker", hot)
    must("reemit_success_coverage_override_", "AC1 override", hot)
    pipe = hot.find("void HotUpdateRegistry::on_reemit_pipeline_call")
    if pipe < 0:
        fails.append("AC1: on_reemit_pipeline_call missing")
    else:
        body = hot[pipe : pipe + 2800]
        must_not(
            "covered = candidates & emit_region_mask_.load",
            "AC1 pipeline must not stamp count ∩ emit_mask",
            body,
        )
        must_not(
            "if (covered == 0)\n                    covered = demoted;",
            "AC1 pipeline must not fall back to full demoted",
            body,
        )
        if "reemit_success_coverage_override_" not in body:
            fails.append("AC1: pipeline must load reemit_success_coverage_override_")
        if "if (covered != 0)" not in body:
            fails.append("AC1: pipeline must store last_success only when covered != 0")
        if "last_reemit_success_region_mask_.store(covered" not in body:
            fails.append("AC1: pipeline must store override into last_reemit_success")
        if "if (demoted != 0)" not in body:
            fails.append("AC1: pipeline still gates stamp on demoted != 0")
        if "Issue #3445" not in body:
            fails.append("AC1: pipeline body missing Issue #3445 cite")
        # Counters still consume the count args (they are not coverage).
        if "reemit_candidates_.fetch_add(candidates" not in body:
            fails.append("AC1: pipeline must still aggregate candidate COUNT")
        if "reemit_success_.fetch_add(successes" not in body:
            fails.append("AC1: pipeline must still aggregate success COUNT")

    must("skip the fallback `covered = demoted` stamp", "AC1 #3413 skip kept", hot)
    must("3445 AC1", "AC1 test", test)

    must("maybe_force_jit_repromote_on_clean_success", "AC2 only_covered", hot)
    must("resolve_force_jit_repromote_only_covered", "AC2 policy", hot)
    must("3445 AC2", "AC2 test", test)

    must("residual_force_mask", "AC3 residual", hot)
    must("playbook_hint_min_dirty_reemit", "AC3 playbook hint", hot)
    must("maybe_coverage_verify_min_dirty", "AC3 min-dirty", hot)
    must("3445 AC3", "AC3 test", test)

    must("note_relower_success_coverage", "AC4 relower hash", hh)
    must("note_relower_success_define", "AC4 define side set", hh)
    must("clear_relower_success_defines", "AC4 side-set clear", hot)
    must("relower_success_region_bit", "AC4 hashed-name helper", hh)
    must("3445 AC4", "AC4 test", test)

    if hot.count("if (demoted != 0)") < 2:
        fails.append(
            "AC5: `if (demoted != 0)` must remain at decide_and_reemit AND "
            "on_reemit_pipeline_call (Soft / idle zero extra stores)"
        )
    must("3445 AC5", "AC5 test", test)

    for marker in ("#3413", "#2895", "#2949", "#2978", "#3026"):
        if marker not in hot:
            fails.append(f"AC6: {marker} upstream contract marker missing")
    if "#3136" not in hot and "#3229" not in hot:
        fails.append("AC6: #3136 / #3229 relower define coverage marker missing")
    must("stamp_aot_reload_consistency_proof_fail_after_force_jit", "AC6 #2845", hot)
    must("3445 AC6", "AC6 test", test)
    must("check_reemit_pipeline_reason_coverage_3445", "AC6 build.py", build)
    must_not("schema-3445", "AC6 no new query key", mutate)
    if (ROOT / "tests" / "compiler" / "test_issue_3445.cpp").is_file():
        fails.append("AC6: forbidden tests/compiler/test_issue_3445.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3445.cpp").is_file():
        fails.append("AC6: forbidden tests/issues/test_issue_3445.cpp")
    if (ROOT / "tests" / "core" / "test_issue_3445.cpp").is_file():
        fails.append("AC6: forbidden tests/core/test_issue_3445.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3445-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3445 reemit_pipeline_reason_coverage:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3445 reemit_pipeline_reason_coverage: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
