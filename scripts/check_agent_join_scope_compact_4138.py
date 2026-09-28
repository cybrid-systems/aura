#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4138: three-plane hygiene — orch:agent-join Done husk skips Scope
# compact. The #3776 compact sites (Scope spawn / join_all / watch_all) run
# compact_done_husks_unlocked_ only on those paths, but the Aura
# orch:agent-join prim resolves Scope-owned handles (resolve_aura_agent
# Scope fallback, #3442) and joins them one at a time — leaving a
# reclaimable-clean Done husk in handles_ / specs_ / restart vectors until
# the next compact site. Long-run high-churn multi-agent hosts accumulate
# ghosts even though find/directory already skip them (#3598/#3527/#3805)
# and the name-table plane erases on find.
# Fix: the join prim captures the resolve plane (scope_hit out-param on a
# resolve_aura_agent overload — same #3442 walk, no new atomic) and calls
# AgentScope::compact_after_single_join() once after Done-path cleanup,
# under ScopeEnterGuard, reusing the compact_done_husks_unlocked_ SSOT (no
# second model). Soft / Off: no compact (append-only contract, #3776 AC5).
# Reclaimed-pending (#2661/#3467) and live slots never match the husk
# predicate and are never compacted away.
#
# AC1 - entry + SSOT: AgentScope::compact_after_single_join() exists in
#       agent_scope.h citing Issue #4138, takes the ScopeEnterGuard and
#       calls the compact_done_husks_unlocked_ SSOT (production-gated, no
#       second gate model); stamp kScopeJoinDoneHuskCompactIssue = 4138.
#       Runtime pin: ac4138_1 (single-join soak — handles_ bounded).
# AC2 - prim wiring + ordering: the orch:agent-join prim body resolves via
#       resolve_aura_agent(ev, name, &join_scope) and calls
#       join_scope->compact_after_single_join() AFTER the last hp->
#       dereference (compact resizes handles_ and invalidates hp); the
#       2-arg #3442 resolve signature is retained. Runtime pin: ac4138_2
#       (RestartN watch after many single joins — indices aligned).
# AC3 - pending safety: slot_is_reclaimable_clean keeps the
#       must_wait/deferred exclusion (#2661/#3467), is_done_path_husk_
#       keeps the ok + reclaimable-clean/abandoned-live shape, and the
#       #3497 name-reuse pending walk in spawn stays intact. Runtime pin:
#       ac4138_3 (must_wait survives the single-join compact).
# AC4 - Soft/Off append-only + no invent: the compact keeps the single
#       production_defaults_active gate (Soft/Off zero cost), no
#       process-global AgentRegistry, no new query:4138 key. Runtime pin:
#       ac4138_4 (Soft keeps the husk).
# AC5 - hosting/wiring: the five ac4138_* runtime ACs are defined AND
#       called in tests/orch/test_join_drain_reclaim.cpp, no
#       tests/**/test_issue_4138.cpp (per #81934), no docs/design/4138-*
#       (per #1655), linter registered in build.py and on the root check
#       allowlist.
#
# Self-test:
#   python3 scripts/check_agent_join_scope_compact_4138.py
from __future__ import annotations

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
        return ""
    depth = 0
    end = open_brace
    for i in range(open_brace, min(open_brace + limit, len(src))):
        c = src[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    return src[start:end]


_FAILS: list[str] = []


def _must(cond: bool, label: str) -> None:
    if not cond:
        _FAILS.append(label)


def main() -> int:
    scope_src = _strip_cpp_comments(_read("src/orch/agent_scope.h"))
    spawn_src = _strip_cpp_comments(_read("src/orch/agent_spawn.h"))
    prim_src = _strip_cpp_comments(_read("src/compiler/evaluator_primitives_agent.cpp"))
    build_src = _read("build.py")
    allowlist = _read("scripts/coverage/root_check_allowlist.txt")
    test_src = _read("tests/orch/test_join_drain_reclaim.cpp")

    # AC1 — entry + SSOT.
    entry = scope_src.find("void compact_after_single_join()")
    _must(entry >= 0, "AC1: AgentScope::compact_after_single_join entry point missing")
    body = _brace_slice(scope_src, entry) if entry >= 0 else ""
    _must('ScopeEnterGuard g(this, "compact_after_single_join")' in body, "AC1: compact runs under ScopeEnterGuard")
    _must(
        "compact_done_husks_unlocked_()" in body,
        "AC1: entry calls the compact_done_husks_unlocked_ SSOT (no second model)",
    )
    _must(
        scope_src.find("kScopeJoinDoneHuskCompactIssue = 4138") >= 0,
        "AC1: stamp kScopeJoinDoneHuskCompactIssue = 4138 missing",
    )
    compact_fn = scope_src.find("compact_done_husks_unlocked_() noexcept")
    _must(compact_fn >= 0, "AC1: compact_done_husks_unlocked_ definition missing")
    compact_body = _brace_slice(scope_src, compact_fn) if compact_fn >= 0 else ""
    _must(
        "production_defaults_active()" in compact_body,
        "AC1: compact SSOT keeps the production gate (Soft/Off append-only)",
    )

    # AC2 — prim wiring + ordering.
    overload = prim_src.find("aura::orch::AgentScope** scope_hit")
    _must(overload >= 0, "AC2: plane-detecting resolve overload missing")
    _must(
        prim_src.find("aura::orch::AgentHandle* resolve_aura_agent(Evaluator& ev, const std::string& name)") >= 0,
        "AC2: 2-arg #3442 resolve signature retained",
    )
    start = prim_src.find('add("orch:agent-join",')
    _must(start >= 0, "AC2: orch:agent-join prim not found")
    nxt = prim_src.find('add("orch:agent-wait-reclaimed",', start)
    _must(nxt > start, "AC2: prim window end not found")
    join_body = prim_src[start:nxt] if (start >= 0 and nxt > start) else ""
    _must(
        "resolve_aura_agent(ev, name, &join_scope)" in join_body, "AC2: prim resolves via the plane-detecting overload"
    )
    _must("join_scope->compact_after_single_join()" in join_body, "AC2: prim calls the Scope compact once")
    last_hp = join_body.rfind("hp->")
    compact_call = join_body.find("join_scope->compact_after_single_join()")
    _must(
        last_hp >= 0 and compact_call > last_hp,
        "AC2: compact must run after the last hp-> deref (resize invalidates hp)",
    )

    # AC3 — pending safety (#2661/#3467/#3497 preserved).
    slot_fn = spawn_src.find("slot_is_reclaimable_clean(const AgentHandle& h)")
    _must(slot_fn >= 0, "AC3: slot_is_reclaimable_clean missing")
    slot_body = _brace_slice(spawn_src, slot_fn) if slot_fn >= 0 else ""
    _must(
        "h.must_wait_reclaimed || h.reclaimed_deferred_cleanup" in slot_body,
        "AC3: reclaimable-clean keeps the pending exclusion (#2661/#3467)",
    )
    husk_fn = scope_src.find("is_done_path_husk_(const AgentHandle& h)")
    _must(husk_fn >= 0, "AC3: is_done_path_husk_ missing")
    husk_body = _brace_slice(scope_src, husk_fn) if husk_fn >= 0 else ""
    _must(
        "slot_is_abandoned_live(h)" in husk_body and "h.ok && aura::orch::slot_is_reclaimable_clean(h)" in husk_body,
        "AC3: husk predicate shape unchanged (abandoned-live + ok clean)",
    )
    _must(scope_src.find("name-reuse-while-reclaimed-pending (#3497)") >= 0, "AC3: #3497 spawn pending walk intact")

    # AC4 — Soft/Off append-only + no invent.
    _must(
        spawn_src.find("class AgentRegistry") < 0
        and scope_src.find("class AgentRegistry") < 0
        and prim_src.find("class AgentRegistry") < 0,
        "AC4: no process-global AgentRegistry",
    )
    _must(join_body.find("query:4138") < 0 and prim_src.find("query:4138") < 0, "AC4: no new query:4138 key")

    # AC5 — hosting / wiring.
    for fn in (
        "ac4138_1_single_join_soak_scope_compact",
        "ac4138_2_restart_watch_indices_aligned",
        "ac4138_3_must_wait_survives_compact",
        "ac4138_4_soft_append_only",
        "ac4138_5_source_cite_linter_no_invent",
    ):
        _must(f"static void {fn}()" in test_src, f"AC5: {fn} not defined in the test TU")
        _must(f"{fn}();" in test_src, f"AC5: {fn} not called by the dispatcher")
    _must(_read("tests/orch/test_issue_4138.cpp") == "", "AC5: tests/orch/test_issue_4138.cpp must not exist")
    _must(_read("tests/issues/test_issue_4138.cpp") == "", "AC5: tests/issues/test_issue_4138.cpp must not exist")
    import glob

    _must(not glob.glob(str(ROOT / "docs" / "design" / "4138-*")), "AC5: no docs/design/4138-* per #1655")
    _must("check_agent_join_scope_compact_4138" in build_src, "AC5: build.py wires the linter")
    _must("check_agent_join_scope_compact_4138.py" in allowlist, "AC5: linter on the root check allowlist")

    if _FAILS:
        for f in _FAILS:
            print(f"FAIL check_agent_join_scope_compact_4138: {f}")
        return 1
    print("check_agent_join_scope_compact_4138: OK (5 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
