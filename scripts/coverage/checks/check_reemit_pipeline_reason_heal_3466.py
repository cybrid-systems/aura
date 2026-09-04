#!/usr/bin/env python3
"""Issue #3466: pipeline stamps last_force_jit_reason group on success.

#3445 banned count∩emit and full-demoted fallback, then left production
only_covered with no writer: last_success stayed 0 unless an Agent set
reemit_success_coverage_override_. Residual force never shrank.

Contract:
  AC1 override wins; else aot_reload_fail_to_force_jit_mask(last_force_jit_reason) ∩ demoted
  AC2 successes>0 + force!=0 + override==0 must not leave last_success==0
  AC3 only_covered still clears only last_success bits
  AC4 residual != 0 still drives min-dirty; playbook hint non-Idle
  AC5 3-candidate emit must not invent bit 0 from the count (#3445 stays closed)
  AC6 Soft / Off / idle force mask: zero extra stores (`demoted != 0`)
  AC7 non-duplicative vs #3445 / #3413 / #2895 / #2949; no new query key;
      no test_issue_3466.cpp

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
    test = (ROOT / "tests" / "compiler" / "test_aot_incremental_reemit.cpp").read_text()
    mutate = (ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp").read_text()

    pipe = hot.find("void HotUpdateRegistry::on_reemit_pipeline_call")
    if pipe < 0:
        fails.append("AC1: on_reemit_pipeline_call missing")
        body = ""
    else:
        body = hot[pipe : pipe + 3200]
    must("Issue #3466", "AC1 marker", body)
    must("last_force_jit_reason_", "AC1 last_force_jit_reason", body)
    must("aot_reload_fail_to_force_jit_mask", "AC1 reason-group map", body)
    must("reemit_success_coverage_override_", "AC1 override still first", body)
    must_not(
        "covered = candidates & emit_region_mask_.load",
        "AC5 pipeline must not stamp count ∩ emit_mask",
        body,
    )
    must_not(
        "if (covered == 0)\n                    covered = demoted;",
        "AC5 pipeline must not fall back to full demoted",
        body,
    )
    if "if (covered == 0)" not in body:
        fails.append("AC1: unset override must fall through to last_force_jit_reason")
    if "if (covered != 0)" not in body:
        fails.append("AC1: store last_success only when covered != 0")
    if "if (demoted != 0)" not in body:
        fails.append("AC6: pipeline still gates stamp on demoted != 0")

    must("skip the fallback `covered = demoted` stamp", "AC7 #3413 skip kept", hot)
    must("Issue #3445", "AC7 #3445 cite", hot)
    must("maybe_force_jit_repromote_on_clean_success", "AC3 only_covered", hot)
    must("residual_force_mask", "AC4 residual", hot)
    must("playbook_hint_min_dirty_reemit", "AC4 playbook hint", hot)

    for marker in (
        "3466 AC1",
        "3466 AC2",
        "3466 AC3",
        "3466 AC4",
        "3466 AC5",
        "3466 AC6",
        "3466 AC7",
    ):
        must(marker, f"{marker} test", test)

    must_not("schema-3466", "AC7 no new query key", mutate)
    if (ROOT / "tests" / "compiler" / "test_issue_3466.cpp").is_file():
        fails.append("AC7: forbidden tests/compiler/test_issue_3466.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3466.cpp").is_file():
        fails.append("AC7: forbidden tests/issues/test_issue_3466.cpp")
    if (ROOT / "tests" / "core" / "test_issue_3466.cpp").is_file():
        fails.append("AC7: forbidden tests/core/test_issue_3466.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3466-*")):
            fails.append(f"AC7: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3466 reemit_pipeline_reason_heal:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3466 reemit_pipeline_reason_heal: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
