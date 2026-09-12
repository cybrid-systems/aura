#!/usr/bin/env python3
"""Issue #3681 source-cite gate: production apply_closure refuses pre-reemit body.

apply_closure's MustDeopt arm and safe-fallback arm washed
must_deopt_before_next_call / epoch-stale into eval_flat of the
pre-reemit body under production defaults (mutate×reemit without a
densify window — #3421 does not fire when nothing moved).

ACs:
  AC1: the MustDeopt arm refuses under production AFTER the #3421 refuse
       — keeps the flag SET, poisons bridge_epoch, tries the IR bridge.
  AC2: the safe-fallback arm refuses under production when the closure's
       define was dirtied this epoch (is_define_dirty_fn_, non-empty
       name), ordered before the #2569/#2578 recover.
  AC3: the #3421 densify-stale refuse is intact (helper + consults).
  AC4: JIT dispatch unchanged; dirty surface reused (no second table).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLAT = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
RUNTIME = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"

H3421 = "production_apply_closure_densify_hard_refuse(arena_, cl_copy,"
PROBE = "typed_audit::production_defaults_active()"
DIRTY = "is_define_dirty_fn_(cl_copy.name)"


def check_ac1(flat: str) -> tuple[bool, str]:
    if flat.count("Issue #3681") < 2:
        return False, f"both arms must cite #3681, found {flat.count('Issue #3681')}"
    h3421 = flat.find(H3421)
    if h3421 < 0:
        return False, "#3421 refuse call missing"
    a1 = flat.find(PROBE, h3421)
    if a1 < 0:
        return False, "Arm 1 production gate missing after the #3421 refuse"
    keep = flat.find("keep must_deopt_before_next_call SET", a1)
    poison = flat.find("it->second.bridge_epoch = 0;", a1)
    bridge = flat.find("invoke_closure_bridge_checked", a1)
    if min(keep, poison, bridge) < 0:
        return False, "Arm 1 keep-flag / poison / bridge shape missing"
    if not (a1 < keep < poison < bridge):
        return False, f"Arm 1 shape misordered: gate@{a1} keep@{keep} poison@{poison} bridge@{bridge}"
    return True, "Arm 1 refuses MustDeopt (keep flag + poison epoch + IR bridge) after #3421"


def check_ac2(flat: str) -> tuple[bool, str]:
    a2 = flat.find(DIRTY)
    if a2 < 0:
        return False, "Arm 2 dirty predicate missing"
    gate = flat.rfind(PROBE, 0, a2)
    namecheck = flat.rfind("!cl_copy.name.empty()", 0, a2)
    body_live = flat.find("const bool body_live = cl_copy.flat", a2)
    if gate < 0 or namecheck < 0 or body_live < 0:
        return False, "Arm 2 gate shape incomplete (probe / name / recover anchor)"
    if not (gate < namecheck < a2 < body_live):
        return False, "Arm 2 gate not ordered before the #2569 recover"
    return True, "Arm 2 refuses dirtied-define epoch-stale before the #2569 recover"


def check_ac3(flat: str) -> tuple[bool, str]:
    for needle in (
        "kApplyClosureDensifyHardRefuseIssue = 3421",
        "resolve_object_remap",
        "last_lifetime_consistency_would_allow",
        "g_last_objects_moved",
    ):
        if needle not in flat:
            return False, f"#3421 helper regressed: {needle} missing"
    return True, "#3421 densify-stale refuse intact (helper + consults unchanged)"


def check_ac4(runtime: str, flat: str) -> tuple[bool, str]:
    if "aura_closure_dispatch_native_checked" not in runtime:
        return False, "JIT dispatch entry missing from runtime"
    if "g_closure_must_deopt" not in runtime:
        return False, "JIT MustDeopt consume missing"
    if "Issue #3681" in runtime:
        return False, "JIT dispatch was touched by #3681 (must stay unchanged, AC4)"
    if "std::unordered_map<std::string, Closure" in flat or "std::map<std::string, Closure" in flat:
        return False, "second closure table risk"
    return True, "JIT dispatch unchanged; dirty surface reused (no second table)"


def run_checks(flat: str, runtime: str) -> list[tuple[str, bool, str]]:
    return [
        ("AC1 MustDeopt refuse shape", *check_ac1(flat)),
        ("AC2 dirty-define refuse", *check_ac2(flat)),
        ("AC3 #3421 refuse intact", *check_ac3(flat)),
        ("AC4 JIT untouched + no second table", *check_ac4(runtime, flat)),
    ]


def report_texts(flat: str, runtime: str) -> int:
    rc = 0
    for name, ok, msg in run_checks(flat, runtime):
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {msg}")
        if not ok:
            rc = 1
    return rc


def self_test() -> int:
    flat = FLAT.read_text(encoding="utf-8")
    runtime = RUNTIME.read_text(encoding="utf-8")
    if report_texts(flat, runtime):
        print("self-test: real files must pass all checks")
        return 1
    mutations = [
        ("AC1 MustDeopt refuse shape", flat.replace("Issue #3681", "Issue #0000"), runtime),
        (
            "AC2 dirty-define refuse",
            flat.replace(DIRTY, "false", 1),
            runtime,
        ),
        (
            "AC3 #3421 refuse intact",
            flat.replace("kApplyClosureDensifyHardRefuseIssue = 3421", "kApplyClosureDensifyHardRefuseIssue = 0", 1),
            runtime,
        ),
        (
            "AC4 JIT untouched + no second table",
            flat,
            runtime.replace("aura_closure_dispatch_native_checked", "aura_closure_dispatch_RENAMED"),
        ),
    ]
    for expect_name, m_flat, m_runtime in mutations:
        results = {(n, ok, msg) for n, ok, msg in run_checks(m_flat, m_runtime)}
        flipped = [(ok, msg) for n, ok, msg in results if n == expect_name]
        ok, msg = flipped[0] if flipped else (True, "check missing")
        if ok:
            print(f"  [FAIL-self-test] mutation did not flip: {expect_name}")
            return 1
        print(f"  [ok] mutation flipped {expect_name}: {msg}")
    print("self-test: all mutations flip their targeted check")
    return 0


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    rc = report_texts(
        FLAT.read_text(encoding="utf-8"),
        RUNTIME.read_text(encoding="utf-8"),
    )
    print("Issue #3681 apply pre-reemit gate FAILED" if rc else "Issue #3681 apply pre-reemit gate OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
