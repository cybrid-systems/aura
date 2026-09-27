#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4115: production join_all / orch:scope-join-all should sweep owed
# Reclaimed-pending. Production join sets must_wait_reclaimed and runs a
# short auto-wait (ensure_reclaimed_cleanup / #3110/#3595). On Timeout the
# body may still be live: reservation/mailbox/name stay owed (#2661 no
# early free). Scope stores handles in handles_ for long-run; if the host
# never calls orch:scope-sweep-reclaimed-pending / ensure_reclaimed_cleanup
# / abandon_reclaimed, slots remain lifecycle=reclaimed-pending, name reuse
# stays denied, and quota-only recycle (#3529/#3841) can free arena without
# clearing plane pending.
# Fix: AgentScope::join_all invokes sweep_reclaimed_pending() once at
# end-of-join (production only; Soft/Off zero-cost), stores the outcome on
# the scope (last_sweep_reclaimed_pending), and the join-all prim publishes
# it as swept-cleaned / swept-still-pending / schema-4115.
#
# AC1 - join_all end-of-join sweep: stamp constant, sweep invocation, and
#       outcome accessor exist in agent_scope.h; the sweep keeps the
#       ensure_reclaimed_cleanup SSOT (no second model).
# AC2 - Soft/Off contract: sweep keeps its production_defaults_active()
#       zero-cost gate; the Soft zero-wait runtime AC stays pinned in the
#       join-drain test.
# AC3 - prim surface: orch:scope-join-all publishes swept-cleaned /
#       swept-still-pending / schema-4115; the #3467 settled-drop gate is
#       intact (sweep outcome captured before the drop).
# AC4 - no drift: #3841 quota-recycle face, must_wait faces, the
#       wait_reclaimed_timeout_total counter, and the explicit #3924
#       sweep prim stay untouched.
# AC5 - hosting/wiring: 6 runtime ACs + runner calls in
#       tests/orch/test_join_drain_reclaim.cpp, prim-surface ACs in
#       tests/orch/test_orch_scope.cpp, no tests/orch/test_issue_4115.cpp,
#       no docs/design/4115-* (per #1655), no new query key, no
#       process-global AgentRegistry; linter registered in build.py and on
#       the root check allowlist.
#
# Self-test:
#   python3 scripts/check_scope_join_sweep_4115.py
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _strip_cpp_comments(src: str) -> str:
    """Remove // line comments and /* block comments */ so substring
    search does not false-positive on prose. Cheap state machine; good
    enough for source-cite checks.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    scope_raw = _read("src/orch/agent_scope.h")
    scope = _strip_cpp_comments(scope_raw)
    prim_raw = _read("src/compiler/evaluator_primitives_agent.cpp")
    prim = _strip_cpp_comments(prim_raw)
    spawn = _strip_cpp_comments(_read("src/orch/agent_spawn.h"))
    join_test = _read("tests/orch/test_join_drain_reclaim.cpp")
    scope_test = _read("tests/orch/test_orch_scope.cpp")
    build_src = _read("build.py")
    allow_src = _read("scripts/coverage/root_check_allowlist.txt")

    # -- AC1: join_all end-of-join sweep --
    if "kJoinAllSweepReclaimedPendingIssue = 4115" not in scope:
        fails.append("AC1: #4115 stamp constant missing from agent_scope.h")
    if "Issue #4115" not in scope_raw:
        fails.append("AC1: join_all sweep must cite Issue #4115")
    if "last_sweep_reclaimed_pending_ = sweep_reclaimed_pending();" not in scope:
        fails.append("AC1: join_all must invoke + store the end-of-join sweep")
    if "SweepReclaimedPendingResult last_sweep_reclaimed_pending()" not in scope:
        fails.append("AC1: sweep outcome accessor missing from AgentScope")
    if "ensure_reclaimed_cleanup(h)" not in scope:
        fails.append("AC1: sweep must keep the ensure_reclaimed_cleanup SSOT")

    # -- AC2: Soft/Off zero-cost contract --
    if "production_defaults_active()" not in scope:
        fails.append("AC2: sweep production gate missing")
    sweep_call = scope.find("last_sweep_reclaimed_pending_ = sweep_reclaimed_pending();")
    if sweep_call < 0 or "Soft / Off" not in scope_raw[max(0, sweep_call - 1200) : sweep_call]:
        fails.append("AC2: join_all sweep must document the Soft/Off zero-cost contract")
    if "ac4115_2_soft_join_all_no_sweep_zero_wait" not in join_test:
        fails.append("AC2: Soft zero-wait runtime AC missing from the join-drain test")

    # -- AC3: prim publishes the sweep outcome --
    for key in ("swept-cleaned", "swept-still-pending", "schema-4115"):
        if f'"{key}"' not in prim_raw:
            fails.append(f"AC3: join-all hash missing {key}")
    if "last_sweep_reclaimed_pending()" not in prim:
        fails.append("AC3: join-all prim must read the scope sweep outcome")
    if "tree_settled_now" not in prim:
        fails.append("AC3: #3467 settled-drop gate must stay intact")

    # -- AC4: no drift on the neighboring contracts --
    if "must_wait_reclaimed" not in spawn:
        fails.append("AC4: agent_spawn.h must_wait_reclaimed face missing")
    if "wait_reclaimed_timeout_total" not in spawn:
        fails.append("AC4: wait_reclaimed_timeout_total counter must stay")
    if "maybe_force_release_reclaimed_quota" not in spawn:
        fails.append("AC4: #3841 quota recycle face must stay")
    if "sweep_reclaimed_pending()" not in prim:
        fails.append("AC4: explicit orch:scope-sweep-reclaimed-pending prim must stay (#3924)")

    # -- AC5: hosting / wiring / no invent --
    defs = re.findall(r"static void (ac4115_\w+)\(\)", join_test)
    calls = re.findall(r"^\s{4}(ac4115_\w+)\(\);", join_test, re.M)
    if len(defs) != 6:
        fails.append(f"AC5: expected 6 ac4115 runtime ACs, found {len(defs)}")
    if set(defs) != set(calls):
        fails.append("AC5: ac4115 ACs must all be called from the batch runner")
    if '"schema-4115"' not in scope_test:
        fails.append("AC5: prim-surface AC (schema-4115) missing from test_orch_scope.cpp")
    if (ROOT / "tests/orch/test_issue_4115.cpp").exists():
        fails.append("AC5: tests/orch/test_issue_4115.cpp must not exist (per #81934)")
    if list((ROOT / "docs" / "design").glob("4115-*")):
        fails.append("AC5: docs/design/4115-* must not exist (per #1655)")
    if "query:4115" in prim_raw or "query:4115" in scope_raw:
        fails.append("AC5: no new query key (query:4115 must not exist)")
    if "class AgentRegistry" in scope:
        fails.append("AC5: no process-global AgentRegistry")
    if "check_scope_join_sweep_4115" not in build_src:
        fails.append("AC5: linter must be registered in build.py")
    if "check_scope_join_sweep_4115.py" not in allow_src:
        fails.append("AC5: linter must be on the root check allowlist")

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("PASS check_scope_join_sweep_4115: all ACs green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
