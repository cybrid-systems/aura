#!/usr/bin/env python3
"""Issue #3641: occupancy ring same-slot collision fail-closed.

Contract (one row per AC):
  AC1  same-slot collision is fail-closed: note_stamped_node takes the
       refuse_foreign_owner guard (foreign node owns the slot / foreign
       owner on the same node / torn or writer-in-flight -> keep
       occupant) and require_effect_for_node_id borrows the slot
       occupant's tenant so the existing foreign on_ref deny fires; the
       #3629 AC1 "collision evicts (bounded, documented)" allow
       assertion is gone
  AC2  non-same-slot exact consult unchanged (#3415/#3629 main path);
       Soft / single-tenant Restricted keep the legacy free note
       (#2056); ring size + consult predicate untouched
  AC3  ACs live in tests/compiler/test_require_effect_auto_isolation.cpp
       (3641 markers: collision deny / owner flip / Soft zero storage /
       audit join / source-cite), each invoked from the runner
  AC4  no tests/**/test_issue_3641.cpp; no docs/design/3641-*
  AC5  build.py wires check_occupancy_collision_failclosed_3641 + root
       allowlist; header carries the 3641 stamp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def forbid(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    prov = _read("src/core/provenance_tracker.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_require_effect_auto_isolation.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: refuse-on-write + query-side collision borrow ───────────────
    must("kNodeOccupancyCollisionIssue = 3641", "AC1 header stamp", prov)
    must("bool refuse_foreign_owner = false", "AC1 note refuse param", prov)
    must("foreign node owns this slot — do not evict", "AC1 evict refuse", prov)
    must("same node, foreign owner — do not flip owner", "AC1 owner refuse", prov)
    must("occupying_stamp_for_node", "AC1 slot-view helper", prov)
    forbid("bounded 1/256 residual", "AC1 evict residual claim gone", prov)
    must("Issue #3641: same-slot collision fail-closed", "AC1 consult cites", sec)
    idx = sec.find("Issue #3641: same-slot collision fail-closed")
    if idx < 0:
        fails.append("AC1: consult collision-borrow block missing")
    else:
        window = sec[idx : idx + 1400]
        if "existing = occ.tenant_id;" not in window:
            fails.append("AC1: occupant tenant not borrowed for the deny")
        if "occ.node_id != static_cast<std::uint32_t>(node_id)" not in window:
            fails.append("AC1: borrow not guarded on foreign node match")
    idx2 = sec.find("Issue #3641: under the consult regime")
    if idx2 < 0:
        fails.append("AC1: stamp_stable_ref refuse wiring missing")
    else:
        window2 = sec[idx2 : idx2 + 1200]
        if "/*refuse_foreign_owner=*/refuse" not in window2:
            fails.append("AC1: note call does not pass refuse flag")
        if "strict_n || (restricted_n && mt_n)" not in window2:
            fails.append("AC1: refuse predicate not the consult regime")
    forbid("same-slot collision evicts (bounded, documented)", "AC1 residual allow gone", test)
    must("3641 AC1: NodeId-only mutate on X denies after same-slot collision", "AC1 test collision deny", test)
    must("3641 AC1: A's same-slot Z stamp did not evict X", "AC1 test no-evict", test)
    # add_mutate gate inherits the deny: NodeId-only still routes through
    # require_effect_for_node_id (the collision borrow lives there).
    must("NodeId-only: stamp current principal then on_ref", "AC1 gate else-branch", mut)
    gate_else = mut.find("NodeId-only: stamp current principal then on_ref")
    if gate_else < 0 or "require_effect_for_node_id" not in mut[gate_else : gate_else + 300]:
        fails.append("AC1: add_mutate NodeId-only no longer routes require_effect_for_node_id")

    # ── AC2: consult semantics unchanged ─────────────────────────────────
    must("kNodeOccupancyRingSlots = 256", "AC2 ring size untouched", prov)
    must("const bool consult = strict || (restricted && mt);", "AC2 consult predicate", sec)
    must("3641 AC2: NodeId-only mutate on X still denies (exact consult)", "AC2 exact-match deny test", test)
    must("3641 AC2: fresh empty-slot NodeId-only allows (caller stamp)", "AC2 no over-deny test", test)
    must("3641 AC4: Soft NodeId-only allows (no consult, #2056/#3415 AC4)", "AC2 Soft unchanged test", test)
    must("3641 AC4: Soft stamp left the ring untouched (zero storage)", "AC2 Soft zero-storage test", test)

    # ── AC3: AC placement + runner invocation ────────────────────────────
    for fn in (
        "ac3641_1_same_slot_collision_fail_closed",
        "ac3641_2_non_same_slot_still_denies",
        "ac3641_3_no_owner_flip_via_stamp_stable_ref",
        "ac3641_4_soft_off_zero_storage",
        "ac3641_5_deny_audit_join_se_typed",
        "ac3641_6_source_cite_and_no_invent",
    ):
        must(fn, "AC3 test function", test)
        if f"{fn}();" not in test:
            fails.append(f"AC3: {fn} not invoked in runner")

    # ── AC4: no new files in forbidden slots ─────────────────────────────
    if _read("tests/issues/test_issue_3641.cpp"):
        fails.append("AC4: tests/issues/test_issue_3641.cpp exists")
    if _read("tests/compiler/test_issue_3641.cpp"):
        fails.append("AC4: tests/compiler/test_issue_3641.cpp exists")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in sorted(design.glob("3641-*")):
            fails.append(f"AC4: docs/design file present: {p.name}")

    # ── AC5: gate wiring ─────────────────────────────────────────────────
    must("check_occupancy_collision_failclosed_3641", "AC5 build.py wires linter", build)
    must("check_occupancy_collision_failclosed_3641.py", "AC5 root allowlist", allow)
    must("Issue #3641", "AC5 build.py rationale", build)

    if fails:
        for f in fails:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print("3641 occupancy-collision fail-closed: all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
