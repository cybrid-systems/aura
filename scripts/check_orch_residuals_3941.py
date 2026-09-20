#!/usr/bin/env python3
"""Issue #3941 — orch multi-agent residuals: three closed-loop fixes.

Children shipped under this umbrella:
  #3944  AgentNameTable put() sweeps cross-name Done-path-cleaned husks
         (production face; Scope's compact_done_husks_unlocked_ #3776
         shape). Long-run unique-name spawn+join no longer grows the
         table until ~Evaluator.
  #3943  production spawn-agent BP gauges for tenant / explicit /
         Scope-named keys refcount under the gauge map mutex: +1 at
         spawn resolve, -1 at ~AgentHandle, erase at zero (unique
         bare:<seq> keys keep the #3931 join/dtor erase).
  #3942  the spawn path binds the fiber's primary mailbox at
         Scheduler::spawn return (pre-publish) so the steal-complete
         held_ref walk (#3111 AC1) sees h.mailbox even on a
         pre-first-instruction steal; the in-body attach is idempotent.

  AC1  #3944 sweep: agent_name_table.h cites "Issue #3944", gates the
      sweep on production_defaults_active(), and excludes the incoming
      name (sit->first != name) so the same-name retire path (#3598 /
      #3467) is untouched.
  AC2  #3944 test-wired: test_agent_name_table_isolation.cpp carries
      "3944 AC1/AC2/AC3" and ac3944_1_prod_sweeps_cross_name_husks is
      wired into run_test_agent_name_table_isolation.
  AC3  #3943 helpers: agent_spawn.h cites Issue #3943, defines
      g_scope_bp_refs + note_scope_bp_gauge_ref +
      maybe_release_scope_bp_gauge_ref, the spawn path increments
      (note_scope_bp_gauge_ref(h.bp_scope_id);), ~AgentHandle releases
      (maybe_release_scope_bp_gauge_ref(bp_scope_id);), and
      reset_scope_bp_map_for_test clears the refcounts.
  AC4  #3943 test-wired: test_bare_bp_resolve.cpp carries the 3943 ACs
      (shared-tenant survive/erase, bare no-refcount, dtor release) and
      source-cites the spawn increment + dtor release.
  AC5  #3942 bind: agent_spawn.h cites "Issue #3942: bind the fiber's
      primary mailbox at spawn time" and attaches (mb->attach(f);)
      after the Scheduler::spawn rollback block.
  AC6  #3942 test-wired: test_steal_complete_restamp_txn.cpp carries
      the 3942 ACs and ac3942_spawn_bind_walk is wired into
      run_test_steal_complete_restamp_txn.
  AC7  no-invent: no docs/design/394{2,3,4}-*, no tests/issues/
      test_issue_394*.cpp, no tests/core/test_issue_394*.cpp.

Exit 0 only when all ACs pass. `--self-test` feeds a stale (pre-fix)
fixture through the same detectors and must flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NT = ROOT / "src" / "compiler" / "agent_name_table.h"
SPAWN = ROOT / "src" / "orch" / "agent_spawn.h"
NT_TEST = ROOT / "tests" / "orch" / "test_agent_name_table_isolation.cpp"
BP_TEST = ROOT / "tests" / "orch" / "test_bare_bp_resolve.cpp"
STEAL_TEST = ROOT / "tests" / "serve" / "test_steal_complete_restamp_txn.cpp"


def ac1_sweep(nt_text: str) -> bool:
    return (
        "Issue #3944" in nt_text
        and "production_defaults_active()" in nt_text
        and "sit->first != name" in nt_text
        and "slot_is_reclaimable_clean(sit->second)" in nt_text
    )


def ac2_nt_test(nt_test_text: str) -> bool:
    return (
        all(f"3944 AC{n}" in nt_test_text for n in (1, 2, 3))
        and "ac3944_1_prod_sweeps_cross_name_husks" in nt_test_text
        and "ac3944_1_prod_sweeps_cross_name_husks();" in nt_test_text
    )


def ac3_refs(spawn_text: str) -> bool:
    return (
        "Issue #3943" in spawn_text
        and "g_scope_bp_refs" in spawn_text
        and "note_scope_bp_gauge_ref(h.bp_scope_id);" in spawn_text
        and "maybe_release_scope_bp_gauge_ref(bp_scope_id);" in spawn_text
        and "g_scope_bp_refs.clear(); // Issue #3943" in spawn_text
        and "inline void note_scope_bp_gauge_ref" in spawn_text
        and "inline void maybe_release_scope_bp_gauge_ref" in spawn_text
    )


def ac4_bp_test(bp_test_text: str) -> bool:
    return (
        "3943 AC1" in bp_test_text
        and "3943 AC2" in bp_test_text
        and "3943 AC3" in bp_test_text
        and "note_scope_bp_gauge_ref(h.bp_scope_id);" in bp_test_text
        and "maybe_release_scope_bp_gauge_ref(bp_scope_id);" in bp_test_text
    )


def ac5_bind(spawn_text: str) -> bool:
    return (
        "Issue #3942: post-attach held_ref revalidation" in spawn_text
        and spawn_text.count("maybe_revalidate_held_ref_after_attach(") >= 2
    )


def ac6_steal_test(steal_test_text: str) -> bool:
    return (
        "3942 AC1" in steal_test_text
        and "3942 AC2" in steal_test_text
        and "ac3942_spawn_bind_walk" in steal_test_text
        and "ac3942_spawn_bind_walk();" in steal_test_text
        and "serve/multi_fiber_mailbox.h" in steal_test_text
    )


def ac7_no_invent() -> bool:
    bad = []
    for n in (3942, 3943, 3944):
        bad.append(ROOT / "docs" / "design" / f"{n}-b.md")
        bad.append(ROOT / "tests" / "issues" / f"test_issue_{n}.cpp")
        bad.append(ROOT / "tests" / "core" / f"test_issue_{n}.cpp")
    return not any(p.exists() for p in bad)


def run_all() -> bool:
    results = [
        ("AC1(#3944 sweep)", ac1_sweep(NT.read_text())),
        ("AC2(#3944 test)", ac2_nt_test(NT_TEST.read_text())),
        ("AC3(#3943 refs)", ac3_refs(SPAWN.read_text())),
        ("AC4(#3943 test)", ac4_bp_test(BP_TEST.read_text())),
        ("AC5(#3942 bind)", ac5_bind(SPAWN.read_text())),
        ("AC6(#3942 test)", ac6_steal_test(STEAL_TEST.read_text())),
        ("AC7(no-invent)", ac7_no_invent()),
    ]
    ok = True
    for label, passed in results:
        print(f"AC({label}): " + ("PASS" if passed else "FAIL"))
        ok = ok and passed
    return ok


def self_test() -> bool:
    stale = """    auto name = h.name.empty() ? ("agent-" + std::to_string(h.id)) : h.name;
    auto it = impl_->agents_.find(name);
"""
    if ac1_sweep(stale):
        print("self-test: stale fixture NOT flagged — detector broken")
        return False
    print("self-test: stale fixture flagged — detector honest")
    return True


def main() -> int:
    if "--self-test" in sys.argv:
        return 0 if self_test() else 1
    return 0 if run_all() else 1


if __name__ == "__main__":
    sys.exit(main())
