#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4136: closed-loop bypass — bare AgentHandle vectors miss the
# Reclaimed sweep after a production Timeout. join_agent / join_agents
# re-derive Reclaimed + must_wait_reclaimed and run a bounded auto-wait
# (#3433/#3012/#3110/#3631), but the existing drain planes are all
# ownership-bound: AgentScope::sweep_reclaimed_pending (#3842, join_all
# end-of-join #4115) only drains Scope-owned handles_, and name-table
# find/put recycle (#3564/#3644) only runs for hosts that resolve by
# name. A host that keeps a bare std::vector<AgentHandle> (or moves
# handles out of Scope) never hits either plane — reservation / mailbox
# / name stay owed until ensure_reclaimed_cleanup / abandon_reclaimed /
# ~AgentHandle.
# Fix: caller-passed sweep_handles(std::span<AgentHandle>) over the
# host's own storage — SAME ensure_reclaimed_cleanup SSOT (no second
# cleanup model), Soft/Off zero-cost production gate, #2661
# no-early-free, no process-global AgentRegistry, no vector reordering.
#
# AC1 - sweep entry + SSOT: sweep_handles(std::span<AgentHandle>) exists
#       in agent_spawn.h citing Issue #4136, keeps the
#       ensure_reclaimed_cleanup SSOT and the wait_reclaimed_body
#       deferred-Done arm; runtime AC ac4136_1 pins the raw-vector soak
#       (reserved_memory_bytes==0, mailbox detached, must_wait cleared).
# AC2 - no drift / no new force path: the sweep body never re-joins
#       (Fiber::join) and never calls abandon / quota-recycle helpers
#       directly (ensure owns those arms); the #2661/#3012/#3297/#3880
#       dtor faces and the #3220 host_forget counter stay; runtime AC
#       ac4136_2 pins the no-sweep soak (metrics fire, dtor safe).
# AC3 - Soft/Off zero-cost: one production_defaults_active() gate before
#       the per-handle loop; runtime AC ac4136_3 pins the zero-wait
#       no-op.
# AC4 - hosting/wiring: 4 ac4136 runtime ACs defined AND called in
#       tests/orch/test_join_drain_reclaim.cpp, no
#       tests/orch/test_issue_4136.cpp (per #81934), no
#       docs/design/4136-* (per #1655), no new query key, no
#       process-global AgentRegistry; linter registered in build.py and
#       on the root check allowlist.
#
# Self-test:
#   python3 scripts/check_caller_sweep_4136.py
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


def _brace_slice(src: str, start: int, limit: int = 8000) -> str:
    """Return src[start:end] where end closes the first { after start."""
    open_brace = src.find("{", start)
    if open_brace < 0:
        return src[start : start + limit]
    depth = 0
    end = min(len(src), start + limit)
    for i in range(open_brace, min(len(src), open_brace + limit)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    return src[start:end]


def main() -> int:
    fails: list[str] = []

    spawn_raw = _read("src/orch/agent_spawn.h")
    spawn = _strip_cpp_comments(spawn_raw)
    join_test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build_src = _read("build.py")
    allow_src = _read("scripts/coverage/root_check_allowlist.txt")

    # -- AC1: sweep entry + SSOT --
    sig = spawn.find("sweep_handles(std::span<AgentHandle>")
    if sig < 0:
        fails.append("AC1: caller-passed sweep_handles(std::span<AgentHandle>) missing")
    if "Issue #4136" not in spawn_raw:
        fails.append("AC1: agent_spawn.h must cite Issue #4136")
    if "struct SweepHandlesResult" not in spawn:
        fails.append("AC1: SweepHandlesResult result struct missing")
    sweep_body = _brace_slice(spawn, sig) if sig >= 0 else ""
    if "ensure_reclaimed_cleanup(h)" not in sweep_body:
        fails.append("AC1: sweep must keep the ensure_reclaimed_cleanup SSOT")
    if "wait_reclaimed_body(h" not in sweep_body:
        fails.append("AC1: sweep must keep the wait_reclaimed_body deferred-Done arm")
    if "ac4136_1_sweep_handles_releases_after_body_exit" not in join_test:
        fails.append("AC1: raw-vector soak runtime AC missing from the join-drain test")

    # -- AC2: no drift / no new force path --
    if sweep_body:
        if "Fiber::join" in sweep_body:
            fails.append("AC2: sweep must not re-join (Fiber::join races residual contracts)")
        for forbidden in (
            "abandon_reclaimed(",
            "maybe_force_release_reclaimed_quota(",
            "maybe_force_recycle_reclaimed_slot(",
        ):
            if forbidden in sweep_body:
                fails.append(f"AC2: sweep must not call {forbidden} directly (ensure SSOT owns it)")
    else:
        fails.append("AC2: sweep body slice unavailable")
    for face in (
        "finish_reclaimed_cleanup_on_dtor",
        "reclaimed_dtor_under_account_total",
        "host_forget_reclaimed_risk_total",
        "must_wait_reclaimed",
        "wait_reclaimed_timeout_total",
    ):
        if face not in spawn:
            fails.append(f"AC2: {face} face must stay in agent_spawn.h")
    if "host_forget_reclaimed_risk_total fires without sweep" not in join_test:
        fails.append("AC2: no-sweep host_forget runtime AC missing")
    if "dtor under-account bumps per owed handle" not in join_test:
        fails.append("AC2: dtor under-account runtime AC missing")
    if "finish_reclaimed_cleanup_on_dtor" not in join_test:
        fails.append("AC2: dtor-safety runtime arm missing from the soak")

    # -- AC3: Soft/Off zero-cost --
    if sweep_body:
        gate = sweep_body.find("production_defaults_active()")
        loop = sweep_body.find("for (auto& h : handles)")
        if gate < 0:
            fails.append("AC3: sweep production gate missing")
        elif loop < 0 or gate > loop:
            fails.append("AC3: production gate must precede the per-handle loop")
    if "ac4136_2_soft_sweep_noop_zero_wait" not in join_test:
        fails.append("AC3: Soft zero-wait runtime AC missing from the join-drain test")

    # -- AC4: hosting / wiring / no invent --
    defs = re.findall(r"static void (ac4136_\w+)\(\)", join_test)
    calls = re.findall(r"^\s{4}(ac4136_\w+)\(\);", join_test, re.M)
    if len(defs) != 3:
        fails.append(f"AC4: expected 3 ac4136 runtime ACs, found {len(defs)}")
    if set(defs) != set(calls):
        fails.append("AC4: ac4136 ACs must all be called from the batch runner")
    if (ROOT / "tests/orch/test_issue_4136.cpp").exists():
        fails.append("AC4: tests/orch/test_issue_4136.cpp must not exist (per #81934)")
    if list((ROOT / "docs" / "design").glob("4136-*")):
        fails.append("AC4: docs/design/4136-* must not exist (per #1655)")
    if "query:4136" in spawn_raw:
        fails.append("AC4: no new query key (query:4136 must not exist)")
    if "class AgentRegistry" in spawn or "struct AgentRegistry" in spawn:
        fails.append("AC4: no process-global AgentRegistry")
    if "check_caller_sweep_4136" not in build_src:
        fails.append("AC4: linter must be registered in build.py")
    if "check_caller_sweep_4136.py" not in allow_src:
        fails.append("AC4: linter must be on the root check allowlist")

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("PASS check_caller_sweep_4136: all ACs green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
