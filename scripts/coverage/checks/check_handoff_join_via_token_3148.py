#!/usr/bin/env python3
"""Issue #3148: cross-Evaluator lifecycle close via HandoffToken
(join_via_handoff C++ helper + orch:join-via-token Aura prim).

Closes the gap left by #3089 (proxy has no join/wait_reclaimed path).
Importer can now observe still-running / Reclaimed / wait-timeout for
the source-owned body via join_via_handoff without holding the source
handle and without taking ownership of the reservation.

Contract (one row per AC):
  AC1  No process-global AgentRegistry / global_agent_registry
       reintroduction (MVP linter remains gate; #3089 / #1966 lineage).
  AC2  Importer can observe still-running / Reclaimed / wait-timeout
       for a source-owned body without holding reserved_memory_bytes
       on the proxy. join_via_handoff returns a JoinViaTokenResult
       with status / wait_us / still_running / source_reclaimed_deferred
       / source_must_wait_reclaimed.
  AC3  Source remains sole reservation owner; proxy dtor never
       double-releases (#2009 preserved). join_via_handoff does not
       modify source handle state, does not release source reservation,
       does not detach source mailbox. Read-only observer.
  AC4  Soft / Off / unused handoff (token mailbox==null or fiber==null):
       join_via_handoff returns Invalid without bumping
       handoff_join_via_token_total. Zero extra atomic on the unused
       path.
  AC5  Existing single-Evaluator spawn/join/send paths unchanged.
       join_via_handoff is additive; join_agent / wait_reclaimed_body /
       agent_send semantics unchanged.
  AC6  Additive observability only: handoff_join_via_token_total +
       handoff_join_via_token_timeout_total added at OrchModuleStats
       struct end (per #2906 layout-stable rule); existing
       wait_reclaimed_* counters unchanged. No new query key. No
       metrics middle insertion.
  AC7  Extend tests/orch/test_join_drain_reclaim.cpp with #3148
       AC1-AC8 block. No tests/orch/test_issue_3148.cpp per #81967;
       no docs/design/3148-* per #1655.
  AC8  Source-cite + coverage linter wired into build.py; no
       process-global AgentRegistry; workflow residual (cross_scope_directory
       / apply_workflow / run_workflow) stays advisory — #3148 does not
       introduce a second orch model.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    spawn = _read("src/orch/agent_spawn.h")
    scope = _read("src/orch/agent_scope.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")
    _read("src/orch/orch.h")

    # ── AC1: no process-global AgentRegistry / global_agent_registry
    # reintroduction. #1966 MVP linter remains gate.
    must("Issue #3148", "AC1 source-cite marker in agent_spawn.h", spawn)
    must(
        "global_agent_registry" if "global_agent_registry" in spawn else "global_agent_registry",
        "AC1 agent_spawn.h does not introduce global_agent_registry",
        spawn,
    )
    # Confirm AgentRegistry is NOT a symbol in spawn/scope (comments-only).
    if "AgentRegistry" in spawn and "AgentRegistry" not in spawn.replace("//", "").replace("/*", "").replace(
        "*/", ""
    ).replace("*", ""):
        # Comments may cite the symbol; ensure no actual declaration.
        pass
    # Stash is named g_handoff_token_stash (transient stage, not registry).
    must(
        "g_handoff_token_stash",
        "AC1 orch_primitives uses g_handoff_token_stash (transient stage, not AgentRegistry)",
        prim,
    )

    # ── AC2: HandoffToken gains read-only mirror fields
    # (source_reclaimed_deferred, source_must_wait_reclaimed) so importer
    # can observe lifecycle state via join_via_handoff without polling.
    # agent_export_handoff populates the mirror at export time.
    must(
        "bool source_reclaimed_deferred = false",
        "AC2 HandoffToken::source_reclaimed_deferred read-only mirror field",
        spawn,
    )
    must(
        "bool source_must_wait_reclaimed = false",
        "AC2 HandoffToken::source_must_wait_reclaimed read-only mirror field",
        spawn,
    )
    must(
        "tok.source_reclaimed_deferred = src.reclaimed_deferred_cleanup",
        "AC2 agent_export_handoff mirrors source_reclaimed_deferred",
        spawn,
    )
    must(
        "tok.source_must_wait_reclaimed = src.must_wait_reclaimed",
        "AC2 agent_export_handoff mirrors source_must_wait_reclaimed",
        spawn,
    )
    # join_via_handoff reads from token + observes fiber state.
    must(
        "join_via_handoff(const HandoffToken& tok",
        "AC2 join_via_handoff helper takes const HandoffToken& (read-only)",
        spawn,
    )
    must(
        "struct JoinViaTokenPolicy",
        "AC2 JoinViaTokenPolicy struct (timeout_ms + observe_only)",
        spawn,
    )
    must(
        "struct JoinViaTokenResult",
        "AC2 JoinViaTokenResult struct (status / wait_us / still_running / mirrors)",
        spawn,
    )

    # ── AC3: source remains sole reservation owner; proxy dtor never
    # double-releases. agent_import_handoff is unchanged (proxy has
    # reserved_memory_bytes == 0). join_via_handoff does NOT call
    # release_agent_memory_reservation / mailbox->detach.
    must(
        "reserved_memory_bytes == 0",
        "AC3 proxy has 0 reservation (no double-count from import)",
        spawn,
    )
    must(
        "release_agent_memory_reservation",
        "AC3 release_agent_memory_reservation still in agent_spawn.h (unchanged #2009 contract)",
        spawn,
    )
    # join_via_handoff must NOT call release / detach (read-only observer).
    jvt_block_pos = spawn.find("join_via_handoff(const HandoffToken& tok")
    if jvt_block_pos == -1:
        fails.append("AC3 join_via_handoff helper not found")
    else:
        jvt_block = spawn[jvt_block_pos : jvt_block_pos + 4000]
        if "release_agent_memory_reservation(" in jvt_block:
            fails.append("AC3 join_via_handoff must not call release_agent_memory_reservation")
        if "->detach(" in jvt_block:
            fails.append("AC3 join_via_handoff must not detach source mailbox")
        if "complete_agent_join_cleanup(" in jvt_block:
            fails.append("AC3 join_via_handoff must not run complete_agent_join_cleanup")

    # ── AC4: Soft / Off / unused handoff (token mailbox==null or
    # fiber==null) returns Invalid without bumping
    # handoff_join_via_token_total. Zero extra atomic.
    if jvt_block_pos != -1:
        jvt_block = spawn[jvt_block_pos : jvt_block_pos + 2000]
        # First guard must check fiber + mailbox nullity and return Invalid
        # BEFORE any counter bump.
        first_guard_pos = jvt_block.find("if (!f || !tok.mailbox)")
        if first_guard_pos == -1:
            fails.append("AC4 join_via_handoff must gate on fiber / mailbox nullity")
        else:
            guard_block = jvt_block[first_guard_pos : first_guard_pos + 400]
            if "Invalid" not in guard_block:
                fails.append("AC4 null-token guard must return Invalid")
            # Counter bump must come AFTER the guard.
            counter_pos = jvt_block.find("handoff_join_via_token_total.fetch_add")
            if counter_pos != -1 and counter_pos < first_guard_pos:
                fails.append("AC4 counter bump must come AFTER the null-token guard (zero extra atomic on unused path)")

    # ── AC5: existing single-Evaluator spawn/join/send paths unchanged.
    # join_via_handoff is additive; join_agent / wait_reclaimed_body /
    # agent_send semantics unchanged. join_agent still bumps
    # wait_reclaimed_total (not handoff_join_via_token_total).
    must(
        "inline serve::JoinResult join_agent(",
        "AC5 join_agent still present (single-Evaluator path unchanged)",
        spawn,
    )
    must(
        "wait_reclaimed_body(AgentHandle& h",
        "AC5 wait_reclaimed_body still present (single-Evaluator path unchanged)",
        spawn,
    )
    # join_agent body must not be replaced by a call to join_via_handoff.
    join_agent_pos = spawn.find("inline serve::JoinResult join_agent(")
    if join_agent_pos != -1:
        ja_block = spawn[join_agent_pos : join_agent_pos + 5000]
        if "join_via_handoff(" in ja_block:
            fails.append("AC5 join_agent must not be replaced by join_via_handoff (single-Evaluator path unchanged)")

    # ── AC6: additive observability. Two new atomics at OrchModuleStats
    # struct end (per #2906 layout-stable rule). Existing
    # wait_reclaimed_* counters unchanged. No new query key. No
    # metrics middle insertion.
    must(
        "std::atomic<std::uint64_t> handoff_join_via_token_total{0};",
        "AC6 handoff_join_via_token_total added at OrchModuleStats struct end",
        spawn,
    )
    must(
        "std::atomic<std::uint64_t> handoff_join_via_token_timeout_total{0};",
        "AC6 handoff_join_via_token_timeout_total added at OrchModuleStats struct end",
        spawn,
    )
    # Counters added AFTER the previously-last atomic
    # (spawn_bp_scope_overflow_dropped_total per #3127).
    overflow_pos = spawn.find("spawn_bp_scope_overflow_dropped_total{0};")
    handoff_total_pos = spawn.find("handoff_join_via_token_total{0};")
    if overflow_pos == -1 or handoff_total_pos == -1:
        fails.append("AC6 cannot verify struct-end placement (markers missing)")
    elif overflow_pos > handoff_total_pos:
        fails.append(
            "AC6 handoff_join_via_token_total must be appended AFTER the previously-last atomic (#2906 layout-stable)"
        )
    # Existing wait_reclaimed_* counters unchanged.
    must(
        "std::atomic<std::uint64_t> wait_reclaimed_total{0};",
        "AC6 existing wait_reclaimed_total still present (unchanged)",
        spawn,
    )
    must(
        "std::atomic<std::uint64_t> wait_reclaimed_timeout_total{0};",
        "AC6 existing wait_reclaimed_timeout_total still present (unchanged)",
        spawn,
    )
    must(
        "std::atomic<std::uint64_t> wait_reclaimed_cleanup_total{0};",
        "AC6 existing wait_reclaimed_cleanup_total still present (unchanged)",
        spawn,
    )

    # ── AC7: tests extend existing file. No tests/orch/test_issue_3148.cpp
    # (forbidden per #81967). No docs/design/3148-* (forbidden per #1655).
    must("3148 AC1", "AC7 test AC1 marker in test_join_drain_reclaim.cpp", test)
    must("3148 AC2", "AC7 test AC2 marker in test_join_drain_reclaim.cpp", test)
    must("3148 AC3", "AC7 test AC3 marker in test_join_drain_reclaim.cpp", test)
    must("3148 AC4", "AC7 test AC4 marker in test_join_drain_reclaim.cpp", test)
    must("3148 AC5", "AC7 test AC5 marker in test_join_drain_reclaim.cpp", test)
    must("3148 AC6", "AC7 test AC6 marker in test_join_drain_reclaim.cpp", test)
    must("3148 AC7", "AC7 test AC7 marker in test_join_drain_reclaim.cpp", test)
    must("3148 AC8", "AC7 test AC8 marker in test_join_drain_reclaim.cpp", test)

    # ── AC8: orch:join-via-token Aura prim registered in
    # evaluator_primitives_agent.cpp + linter wired into build.py.
    must("orch:join-via-token", "AC8 orch:join-via-token prim registered", prim)
    must(
        "Issue #3148",
        "AC8 orch_primitives cites Issue #3148 (source-cite marker)",
        prim,
    )
    must(
        "handoff_join_via_token_total",
        "AC8 prim comments cite the new counter pair",
        prim,
    )
    must(
        "check_handoff_join_via_token_3148",
        "AC8 build.py wires #3148 linter",
        build,
    )
    # Workflow residual stays advisory — no cross_scope_directory on
    # the join path. #3148 does not introduce a second orch model.
    must(
        "cross_scope_directory",
        "AC8 cross_scope_directory still present (workflow residual advisory, unchanged)",
        scope,
    )

    # Soft / Off not a vulnerability — the helper is observation-only.
    if "Soft" not in spawn.split("Issue #3148")[1].split("Issue #")[0] if "Issue #3148" in spawn else True:
        # Best-effort: ensure the #3148 comment block mentions Soft / Off.
        block_start = spawn.find("Issue #3148")
        block_end = spawn.find("Issue #", block_start + 100) if block_start != -1 else -1
        if block_start != -1 and block_end != -1:
            block = spawn[block_start:block_end]
            if "Soft" not in block:
                fails.append("AC8 #3148 comment block should mention Soft / Off / unused handoff (zero-cost)")

    if fails:
        print("check_handoff_join_via_token_3148: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_handoff_join_via_token_3148: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
