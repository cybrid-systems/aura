#!/usr/bin/env python3
"""Issue #3679 source-cite gate: compact_sweep EnvFrame Guard/scan order.

compact_sweep ran the EnvFrame Guard helper + densify ownership scan at
ENTRY while the helper comment claimed post-remap-table ordering — the
scan walked pre-compact slots and Soft live_compact could bump the arena
gen / remap pins with no following restamp unless invalidates_pins fired.

ACs (source-cite on src/compiler/evaluator_gc.cpp):
  AC1: no Guard-helper call between compact_sweep entry and the defer
       predicate (entry-position scan removed).
  AC2: exactly one helper call site, after live_compact(Soft) and before
       the unified restamp (post-compact scan ordering).
  AC3: restamp condition includes remapped_pins > 0; the panic/ffi defer
       early-return metric bumps still precede any compact work.
  AC4: the helper comment cites #3679 and states the post-compact scope.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "src" / "compiler" / "evaluator_gc.cpp"

HELPER_CALL = "(void)run_envframe_lifetime_guard_compact_sweep_helper(*this);"
FN_SIG = "CompactSweepResult Evaluator::compact_sweep(void* sweep_buffers) {"
DEFER = "should_defer_destructive_gc()"
LIVE_COMPACT = "live_compact(aura::ast::LiveCompactMode::Soft)"
RESTAMP = "unified_restamp_after_boundary(UnifiedRestampSite::Densify)"
COND = "lc.invalidates_pins || lc.remapped_pins > 0"
HELPER_DEF = "static int run_envframe_lifetime_guard_compact_sweep_helper(Evaluator& ev) {"


def check_ac1(body: str) -> tuple[bool, str]:
    sig = body.find(FN_SIG)
    defer = body.find(DEFER)
    if sig < 0 or defer < 0:
        return False, "compact_sweep signature or defer predicate missing"
    if HELPER_CALL in body[sig:defer]:
        return False, "Guard-helper call still present between entry and defer predicate"
    return True, "no Guard/scan before the defer predicate (entry call removed)"


def check_ac2(body: str) -> tuple[bool, str]:
    n = body.count(HELPER_CALL)
    if n != 1:
        return False, f"expected exactly 1 helper call site, found {n}"
    lc = body.find(LIVE_COMPACT)
    restamp = body.find(RESTAMP)
    call = body.find(HELPER_CALL)
    if min(lc, restamp, call) < 0:
        return False, "missing live_compact / restamp / call anchor"
    if not (lc < call < restamp):
        return False, f"order broken: live_compact@{lc} call@{call} restamp@{restamp}"
    return True, "single call site ordered live_compact -> Guard/scan -> unified restamp"


def check_ac3(body: str) -> tuple[bool, str]:
    if COND not in body:
        return False, "restamp condition does not include remapped_pins > 0"
    defer = body.find(DEFER)
    if defer < 0:
        return False, "defer predicate missing"
    for metric in ("ffi_defer_because_pin_total", "gc_blocked_by_panic_total"):
        if metric not in body:
            return False, f"defer metric bump {metric} missing (defer face regressed)"
    call = body.find(HELPER_CALL)
    if call < 0 or call < defer:
        return False, "Guard/scan missing or runs before the defer predicate"
    return True, "restamp fires on remapped_pins; defer early-return intact before any scan"


def check_ac4(body: str) -> tuple[bool, str]:
    helper_def = body.find(HELPER_DEF)
    if helper_def < 0:
        return False, "helper definition missing"
    comment = body[max(0, helper_def - 900) : helper_def]
    if "#3679" not in comment:
        return False, "helper comment does not cite #3679"
    if "after pair compact" not in comment:
        return False, "helper comment does not state the post-compact scope"
    if "#3679" not in body:
        return False, "evaluator_gc.cpp does not cite #3679"
    return True, "helper comment matches the post-compact position (#3679)"


CHECKS = [
    ("AC1 entry-scan removed", check_ac1),
    ("AC2 post-compact scan order", check_ac2),
    ("AC3 restamp+defer face", check_ac3),
    ("AC4 helper comment", check_ac4),
]


def run_checks(body: str) -> list[tuple[str, bool, str]]:
    return [(name, *fn(body)) for name, fn in CHECKS]


def report(body: str) -> int:
    rc = 0
    for name, ok, msg in run_checks(body):
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {msg}")
        if not ok:
            rc = 1
    return rc


def self_test() -> int:
    real = TARGET.read_text(encoding="utf-8")
    if report(real):
        print("self-test: real file must pass all checks")
        return 1
    # Mutation matrix: each targeted mutation must flip exactly its check.
    mutations = [
        ("AC1 entry-scan removed", real.replace(FN_SIG, FN_SIG + "\n    " + HELPER_CALL, 1)),
        ("AC2 post-compact scan order", real.replace(HELPER_CALL, "// moved", 1)),
        ("AC3 restamp+defer face", real.replace(COND, "lc.invalidates_pins", 1)),
        ("AC4 helper comment", real.replace("after pair compact", "before pair compact", 1)),
    ]
    for expect_name, mutated in mutations:
        results = dict((n, (ok, msg)) for n, ok, msg in run_checks(mutated))
        ok, msg = results[expect_name]
        if ok:
            print(f"  [FAIL-self-test] mutation did not flip: {expect_name}")
            return 1
        print(f"  [ok] mutation flipped {expect_name}: {msg}")
    print("self-test: all mutations flip their targeted check")
    return 0


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    if not TARGET.exists():
        print(f"missing {TARGET}")
        return 1
    rc = report(TARGET.read_text(encoding="utf-8"))
    print("Issue #3679 source-cite gate FAILED" if rc else "Issue #3679 source-cite gate OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
