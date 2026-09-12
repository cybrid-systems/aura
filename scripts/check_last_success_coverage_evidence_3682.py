#!/usr/bin/env python3
"""Issue #3682 source-cite gate: last_success coverage needs positive evidence.

on_reemit_pipeline_call stamped aot_reload_fail_to_force_jit_mask(
last_force_jit_reason) & demoted on ANY successes>0 — the global
last-fail reason read as "this emit healed that reason", so an
unrelated define's cascade collapsed residual_force_mask and let
production only_covered re-promote a never-re-emitted region.

ACs:
  AC1: pipeline stamps last_success from the Agent override only —
       the last_force_jit_reason inference is gone from
       on_reemit_pipeline_call.
  AC2: override path intact (note_reemit_success_coverage sticky +
       immediate stamp; #3466 semantics preserved).
  AC3: only_covered re-promote gates zero coverage — no #2502
       wholesale fall-through when last_success == 0 (gate ordered
       before the partial-clear branch).
  AC4: Soft / force_mask==0 short-circuits retained (zero-cost idle).
  AC5: tests extended (repromote + incremental-reemit + cascade-dirty
       cite #3682; no test_issue_3682.cpp; no docs/design/3682-*).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REG = ROOT / "src" / "compiler" / "hot_update_registry.cpp"
REPROMOTE = ROOT / "tests" / "compiler" / "test_force_jit_repromote.cpp"
INCREMENTAL = ROOT / "tests" / "compiler" / "test_aot_incremental_reemit.cpp"
CASCADE = ROOT / "tests" / "compiler" / "test_hot_update_cascade_dirty_reemit.cpp"


def fn_body(src: str, header: str) -> str:
    i = src.find(header)
    if i < 0:
        return ""
    j = src.find("\nvoid HotUpdateRegistry::", i + 1)
    j2 = src.find("\nbool HotUpdateRegistry::", i + 1)
    ends = [x for x in (j, j2) if x >= 0]
    return src[i : min(ends) if ends else len(src)]


def check_ac1(src: str) -> tuple[bool, str]:
    body = fn_body(src, "void HotUpdateRegistry::on_reemit_pipeline_call")
    if not body:
        return False, "on_reemit_pipeline_call body missing"
    # The removed inference had a unique shape (fail-local + mask & demoted);
    # other functions legitimately read last_force_jit_reason_ (retry-reason
    # preservation, repromote correlation, fail stamping).
    if "aot_reload_fail_to_force_jit_mask(fail)" in src:
        return False, "last_force_jit_reason inference still present (fail & demoted)"
    if "reemit_success_coverage_override_.load" not in body:
        return False, "override load missing"
    if "last_reemit_success_region_mask_.store(covered" not in body:
        return False, "covered stamp missing"
    if "Issue #3682" not in body:
        return False, "body must cite #3682"
    return True, "pipeline stamps last_success from the Agent override only"


def check_ac2(src: str) -> tuple[bool, str]:
    i = src.find("void HotUpdateRegistry::note_reemit_success_coverage")
    if i < 0:
        return False, "note_reemit_success_coverage missing"
    body = src[i : i + 900]
    if "reemit_success_coverage_override_.store" not in body:
        return False, "sticky override store missing"
    if "last_reemit_success_region_mask_.store" not in body:
        return False, "immediate last-success stamp missing"
    return True, "override path intact (#3466 semantics preserved)"


def check_ac3(src: str) -> tuple[bool, str]:
    gate = src.find("if (partial && last_cov == 0)")
    partial = src.find("if (partial && last_cov != 0)")
    if gate < 0 or partial < 0:
        return False, "zero-coverage gate / partial branch missing"
    if not (gate < partial):
        return False, "zero-coverage gate must precede the partial-clear branch"
    window = src[gate:partial]
    if "Issue #3682" not in window:
        return False, "zero-coverage gate must cite #3682"
    if "force_jit_stable_successes_.store(0" not in window or "return;" not in window:
        return False, "gate must reset the streak and return (no fall-through)"
    return True, "only_covered re-promote refuses zero coverage (no wholesale fall-through)"


def check_ac4(src: str) -> tuple[bool, str]:
    body = fn_body(src, "void HotUpdateRegistry::on_reemit_pipeline_call")
    if "else if (force_jit_regions_mask_.load(std::memory_order_relaxed) != 0)" not in body:
        return False, "zero-success force-mask branch must stay"
    head = src.find("void HotUpdateRegistry::maybe_force_jit_repromote_on_clean_success")
    head_body = src[head : head + 900] if head >= 0 else ""
    if "if (mask == 0)" not in head_body:
        return False, "repromote mask==0 idle short-circuit missing"
    return True, "Soft / mask==0 short-circuits retained"


def check_ac5() -> tuple[bool, str]:
    rep = REPROMOTE.read_text() if REPROMOTE.exists() else ""
    inc = INCREMENTAL.read_text() if INCREMENTAL.exists() else ""
    cas = CASCADE.read_text() if CASCADE.exists() else ""
    if "ac3682_idle_cascade_no_coverage" not in rep:
        return False, "repromote test must carry the #3682 runtime AC"
    if "ac3682_idle_cascade_no_coverage();" not in rep:
        return False, "repromote test must call the #3682 AC"
    if "#3682" not in inc:
        return False, "incremental-reemit fixture must cite #3682"
    if "invents no coverage (#3682)" not in cas:
        return False, "cascade-dirty fixture must cite #3682"
    if (ROOT / "tests" / "compiler" / "test_issue_3682.cpp").exists():
        return False, "forbidden tests/**/test_issue_3682.cpp (per #81934)"
    if (ROOT / "docs" / "design" / "3682-last-success-evidence.md").exists():
        return False, "forbidden docs/design/3682-* (per #1655)"
    return True, "tests extended (no new test file, no design doc)"


def main() -> int:
    if not REG.exists():
        print(f"FAIL: cannot read {REG}")
        return 1
    src = REG.read_text()
    ok = True
    for name, fn in (
        ("AC1", lambda: check_ac1(src)),
        ("AC2", lambda: check_ac2(src)),
        ("AC3", lambda: check_ac3(src)),
        ("AC4", lambda: check_ac4(src)),
        ("AC5", check_ac5),
    ):
        good, msg = fn()
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good
    if not ok:
        print("Issue #3682 last-success evidence linter: FAIL")
        return 1
    print("Issue #3682 last-success evidence linter: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
