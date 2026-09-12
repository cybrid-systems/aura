#!/usr/bin/env python3
"""Issue #3680 source-cite gate: compact_env_frames dual-epoch bump owner-scope.

The CompilerService ctor hook installed the compact_env_frames dual-epoch
bump as a naked joint writer (bridge_epoch + g_aot_table_epoch fetch_add +
C-bridge dual-write) — under production multi-eval every EnvFrame compact
force-staled ALL peer live closures of unrelated defines.

ACs:
  AC1: the hook cites #3680 and orders owner TLS guard -> facade predicate
       -> bumps inside install_bridge_epoch_bump_fn.
  AC2: the skip predicate is production probe AND
       aura_aot_bump_will_be_owner_scoped, returning before any bump; the
       weak probe declaration carries a null-check fail-open.
  AC3: the #3605 facade surface is untouched (same predicate consulted,
       owner-scoped attribution armed, name-level peer bits preserved).
  AC4: no new epoch domain (hook owns no fetch_add) and the four shared
       bridge_epoch_bump_fn_ call sites are preserved.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERVICE = ROOT / "src" / "compiler" / "service.ixx"
REGISTRY = ROOT / "src" / "compiler" / "hot_update_registry.cpp"
ENV = ROOT / "src" / "compiler" / "evaluator_env.cpp"
EVAL = ROOT / "src" / "compiler" / "evaluator.ixx"

HOOK_ANCHOR = "install_bridge_epoch_bump_fn([](void* svc)"
GUARD_SET = "aura_aot_set_reemit_owner_eval"
PREDICATE = "aura_aot_bump_will_be_owner_scoped"
PROBE = "aura_production_defaults_active_probe"
BUMP = "s->bump_bridge_epoch()"
TABLE = "aura_aot_bump_func_table_epoch()"


def check_ac1(svc: str) -> tuple[bool, str]:
    if "Issue #3680" not in svc:
        return False, "service.ixx does not cite #3680"
    hook = svc.find(HOOK_ANCHOR)
    if hook < 0:
        return False, "compact bump hook missing"
    guard = svc.find(GUARD_SET, hook)
    pred = svc.find(PREDICATE, hook)
    bump = svc.find(BUMP, hook)
    table = svc.find(TABLE, hook)
    if min(guard, pred, bump, table) < 0:
        return False, "guard / predicate / bumps missing from hook"
    if not (guard < pred < bump < table):
        return False, f"hook order broken: guard@{guard} pred@{pred} bump@{bump} table@{table}"
    return True, "hook orders owner TLS guard -> facade predicate -> joint bumps"


def check_ac2(svc: str) -> tuple[bool, str]:
    hook = svc.find(HOOK_ANCHOR)
    if hook < 0:
        return False, "compact bump hook missing"
    pred_at = svc.find(PREDICATE, hook)
    hook_end = svc.find("});", pred_at) if pred_at >= 0 else -1
    if hook_end < 0:
        return False, "hook body unbounded"
    body = svc[hook:hook_end]
    if PROBE not in body:
        return False, "production probe missing from skip predicate"
    if "!= nullptr" not in body:
        return False, "weak probe null-check missing (fail-open)"
    if PREDICATE not in body:
        return False, "facade predicate missing from skip"
    ret = body.find("return;", body.find(PREDICATE))
    bump = body.find(BUMP)
    if ret < 0 or bump < 0 or ret > bump:
        return False, "skip return not ordered before the joint bumps"
    return True, "probe+predicate skip returns before any process-clock bump"


def check_ac3(reg: str) -> tuple[bool, str]:
    checks = (
        "c_clocks_owner_skipped = aura_aot_bump_will_be_owner_scoped()",
        "aura_aot_note_cross_eval_hard_owner_scoped()",
        "aura_aot_mark_peer_jit_name_soft_stale(name)",
    )
    for needle in checks:
        if needle not in reg:
            return False, f"facade surface regressed: {needle} missing"
    return True, "#3605/#3300 facade parity surface preserved"


def check_ac4(svc: str, env: str, ev: str) -> tuple[bool, str]:
    hook = svc.find(HOOK_ANCHOR)
    pred_at = svc.find(PREDICATE, hook) if hook >= 0 else -1
    hook_end = svc.find("});", pred_at) if pred_at >= 0 else -1
    if hook < 0 or hook_end < 0:
        return False, "hook body unbounded"
    if "fetch_add" in svc[hook:hook_end]:
        return False, "hook owns a fetch_add — second epoch domain risk"
    callers = env.count("bridge_epoch_bump_fn_(compiler_service_)")
    callers += ev.count("bridge_epoch_bump_fn_(compiler_service_)")
    if callers < 4:
        return False, f"expected >=4 shared hook call sites, found {callers}"
    return True, "hook owns no fetch_add; 4 shared call sites preserved"


def run_checks(svc: str, reg: str, env: str, ev: str) -> list[tuple[str, bool, str]]:
    return [
        ("AC1 hook order + cite", *check_ac1(svc)),
        ("AC2 skip predicate", *check_ac2(svc)),
        ("AC3 facade parity", *check_ac3(reg)),
        ("AC4 no new epoch domain + call sites", *check_ac4(svc, env, ev)),
    ]


def report_texts(svc: str, reg: str, env: str, ev: str) -> int:
    rc = 0
    for name, ok, msg in run_checks(svc, reg, env, ev):
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {msg}")
        if not ok:
            rc = 1
    return rc


def self_test() -> int:
    svc, reg, env, ev = (
        SERVICE.read_text(encoding="utf-8"),
        REGISTRY.read_text(encoding="utf-8"),
        ENV.read_text(encoding="utf-8"),
        EVAL.read_text(encoding="utf-8"),
    )
    if report_texts(svc, reg, env, ev):
        print("self-test: real files must pass all checks")
        return 1
    mutations = [
        (
            "AC1 hook order + cite",
            svc.replace("Issue #3680", "Issue #0000"),
            reg,
            env,
            ev,
        ),
        ("AC2 skip predicate", svc.replace(PREDICATE, "0"), reg, env, ev),
        (
            "AC3 facade parity",
            svc,
            reg.replace(
                "c_clocks_owner_skipped = aura_aot_bump_will_be_owner_scoped()",
                "auto unused = 0;",
                1,
            ),
            env,
            ev,
        ),
        (
            "AC4 no new epoch domain + call sites",
            svc.replace(
                "            s->bump_bridge_epoch();",
                "            g_hook_fetch_add.fetch_add(1);\n            s->bump_bridge_epoch();",
                1,
            ),
            reg,
            env,
            ev,
        ),
    ]
    for expect_name, m_svc, m_reg, m_env, m_ev in mutations:
        results = {(n, ok, msg) for n, ok, msg in run_checks(m_svc, m_reg, m_env, m_ev)}
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
        SERVICE.read_text(encoding="utf-8"),
        REGISTRY.read_text(encoding="utf-8"),
        ENV.read_text(encoding="utf-8"),
        EVAL.read_text(encoding="utf-8"),
    )
    print("Issue #3680 compact owner-scope gate FAILED" if rc else "Issue #3680 compact owner-scope gate OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
