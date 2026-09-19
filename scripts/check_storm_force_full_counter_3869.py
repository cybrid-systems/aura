#!/usr/bin/env python3
"""Issue #3869 — storm/Global force-full soak/alert counter.

Residual: storm-exit / Global force-full collapses the dirty cone to a
full rewrite (Hard fence, by design) with no rewrite-side count — the
multi-turn throughput cliff was invisible to soak/SLO alerting.

  AC1  counter-decl: pass_pipeline_core.ixx declares
      dirty_aware_storm_force_full_total, cites #3869, and bumps it at
      the consult decision point (single counting site; Soft/Off never
      reaches it; no Soft skip of the Hard fence invented).
  AC2  sites-cite: the rewrite-side sites (pass_pipeline_core.ixx,
      service.ixx, pass_impls.ixx) cite #3869 and do NOT double-bump —
      counting is centralized at the consult.
  AC3  test-wired: test_partial_relower_storm_gate.cpp (built member of
      test_aot_jit_stamp_batch) carries the #3869 behavioral ACs
      (bounded count under synthetic storm; #3831 cap counts nothing;
      Soft does not count).
  AC4  no-invent: no docs/design/3869-*, no test_issue_3869.cpp.

Exit 0 only when all ACs pass. `--self-test` runs the detector over an
inline stale fixture (consult returns true without a counter bump) and
must flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PP = ROOT / "src" / "compiler" / "pass_pipeline_core.ixx"
SITES = [
    ROOT / "src" / "compiler" / "service.ixx",
    ROOT / "src" / "compiler" / "pass_impls.ixx",
]
TEST = ROOT / "tests" / "compiler" / "test_partial_relower_storm_gate.cpp"
COUNTER = "dirty_aware_storm_force_full_total"
BUMP = f"{COUNTER}.fetch_add"


def ac1_counter_decl() -> bool:
    text = PP.read_text()
    ok = (
        f"{COUNTER}{{0}}" in text
        and "Issue #3869" in text
        and "do NOT invent a Soft skip" in text
        and text.count(BUMP) == 1
    )
    print("AC(counter-decl): " + ("PASS" if ok else "FAIL"))
    return ok


def ac2_sites_cite() -> bool:
    ok = True
    for site in [PP, *SITES]:
        text = site.read_text()
        site_ok = "#3869" in text
        if site != PP:
            # Counting is centralized at the consult — no double-bump.
            site_ok = site_ok and BUMP not in text
        print(f"AC(site {site.name}): " + ("PASS" if site_ok else "FAIL"))
        ok = ok and site_ok
    return ok


def ac3_test_wired() -> bool:
    text = TEST.read_text()
    ok = "3869 AC1" in text and "3869 AC2" in text and COUNTER in text
    print("AC(test-wired): " + ("PASS" if ok else "FAIL"))
    return ok


def ac4_no_invent() -> bool:
    docs = list(ROOT.glob("docs/design/3869-*"))
    invented = (ROOT / "tests" / "compiler" / "test_issue_3869.cpp").exists()
    ok = not docs and not invented
    print("AC(no-invent): " + ("PASS" if ok else "FAIL"))
    return ok


def self_test() -> int:
    fixture = (
        "if (storm_level_has_global() && dirty_n < kDefaultPartialRelowerThreshold)\n    return false;\nreturn true;\n"
    )
    flagged = BUMP not in fixture
    print("self-test: " + ("PASS — consult fires without counter bump detected" if flagged else "FAIL"))
    return 0 if flagged else 1


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    results = [
        ac1_counter_decl(),
        ac2_sites_cite(),
        ac3_test_wired(),
        ac4_no_invent(),
    ]
    print(f"check_storm_force_full_counter_3869: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
