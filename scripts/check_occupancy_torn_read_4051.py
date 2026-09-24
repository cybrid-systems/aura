#!/usr/bin/env python3
"""Issue #4051: occupancy seqlock torn read is Uncertain, not Empty.

Contract (one row per AC):
  AC1  third read state exists: provenance_tracker.hh declares
       NodeOccupancyRead {Stable, Uncertain} + occupancy_read_state_for_node
       (odd seq / torn seq -> Uncertain); the collapsed readers
       (existing_stamp_for_node / occupying_stamp_for_node) keep their
       read-path 0 / {} contract
  AC2  the write path consults it: require_effect_for_node_id denies
       Uncertain through the existing IsolationDeny face
       (check_workspace_isolation + nodeid_only_entry_prevented_total)
       BEFORE any caller stamp, and the consult predicate / ring / #3641
       collision borrow / #4039 branch are untouched
  AC3  ACs live in tests/compiler/test_require_effect_auto_isolation.cpp
       (4051 markers: torn-read deny / occupant preserved / stable reads /
       Soft skip / source-cite), dispatched from the batch runner
  AC4  no tests/**/test_issue_4051.cpp; no docs/design/4051-*
  AC5  build.py wires check_occupancy_torn_read_4051 + root allowlist

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
    test = _read("tests/compiler/test_require_effect_auto_isolation.cpp")
    batch = _read("tests/compiler/test_security_capability_batch.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: third read state in the header ──────────────────────────────
    must("Issue #4051", "AC1 header cites issue", prov)
    must("enum class NodeOccupancyRead", "AC1 read-state enum", prov)
    must("Stable,", "AC1 Stable member", prov)
    must("Uncertain,", "AC1 Uncertain member", prov)
    must("occupancy_read_state_for_node", "AC1 read-state helper", prov)
    idx = sec.find("Issue #4051")
    if idx < 0:
        fails.append("AC2: consult site does not cite #4051")
    else:
        window = sec[idx : idx + 1400]
        must("NodeOccupancyRead::Uncertain", "AC2 consult denies Uncertain", window)
        must("check_workspace_isolation(caller, /*ref_tenant=*/0, req_bits, op)", "AC2 IsolationDeny face", window)
        must("nodeid_only_entry_prevented_total", "AC2 counter bump", window)
        if "return false;" not in window:
            fails.append("AC2: Uncertain branch does not fail closed before stamp")
    # collapsed reader contract (read path) untouched
    must("// torn read — treat as miss (caller-stamp fallback)", "AC1 reader contract", prov)
    must("// torn read — not an empty slot", "AC1 third-state note", prov)

    # ── AC2: consult semantics / ring / neighbours untouched ─────────────
    must("kNodeOccupancyRingSlots = 256", "AC2 ring size untouched", prov)
    must("const bool consult = strict || (restricted && mt);", "AC2 consult predicate", sec)
    must("collision_borrow", "AC2 #4039 borrow intact", sec)
    must("occupying_stamp_for_node", "AC2 #3641 slot view intact", prov)
    forbid(
        "NodeOccupancyRead::Uncertain",
        "AC2 no state leak into readers",
        prov[: prov.find("enum class NodeOccupancyRead")],
    )

    # ── AC3: test placement + dispatch ───────────────────────────────────
    for fn in (
        "ac4051_1_torn_read_denies_before_stamp",
        "ac4051_2_stable_reads_unchanged",
        "ac4051_3_soft_off_no_extra_branch",
        "ac4051_4_source_cite_and_no_invent",
    ):
        must(fn, "AC3 test function", test)
        if f"{fn}();" not in test:
            fails.append(f"AC3: {fn} not invoked in a runner")
    must("run_test_occupancy_torn_read_4051", "AC3 batch runner", test)
    must("run_test_occupancy_torn_read_4051", "AC3 dispatcher extern/route", batch)
    must("4051 AC1: torn-read NodeId write denies", "AC3 torn deny assertion", test)
    must("4051 AC1: occupant tenant still A (7)", "AC3 occupant preserved assertion", test)
    must("4051 AC2: stable foreign hit still denies (on_ref face)", "AC3 stable-hit assertion", test)
    must("4051 AC3: Soft path does not bump nodeid_only_entry_prevented", "AC3 Soft zero-branch assertion", test)

    # ── AC4: no new files in forbidden slots ─────────────────────────────
    if _read("tests/issues/test_issue_4051.cpp"):
        fails.append("AC4: tests/issues/test_issue_4051.cpp exists")
    if _read("tests/compiler/test_issue_4051.cpp"):
        fails.append("AC4: tests/compiler/test_issue_4051.cpp exists")
    if _read("tests/core/test_issue_4051.cpp"):
        fails.append("AC4: tests/core/test_issue_4051.cpp exists")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in sorted(design.glob("4051-*")):
            fails.append(f"AC4: docs/design file present: {p.name}")

    # ── AC5: gate wiring ─────────────────────────────────────────────────
    must("check_occupancy_torn_read_4051", "AC5 build.py wires linter", build)
    must("Issue #4051", "AC5 build.py rationale", build)
    must("check_occupancy_torn_read_4051.py", "AC5 root allowlist", allow)

    if fails:
        for f in fails:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print("4051 occupancy torn-read fail-closed: all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
