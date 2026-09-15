#!/usr/bin/env python3
"""Issue #3778: Occurrence/pre-persist/densify mid joins Typed/SE/grant SSOT.

Residual: outermost-pre-persist + Occurrence persist + densify/steal
TypeLinear stamps used Evaluator::defuse_version_ (EnvFrame freshness),
while TypedMutationAudit / SecurityEvent / CapabilityGrant / WAL join via
resolve_audit_mutation_id / join_audit_and_se_mid / cp.audit_mid
(WorkspaceEpoch Mutation). Agents joining Occurrence / densify proof mid
to the Typed trail could miss or false-correlate.

Contract (one row per AC):
  AC1  Production/Full outermost success: persist + pre-persist use
       cp.audit_mid / session_mid / join_audit_and_se_mid — never
       defuse_version_ as the join mid; mid==0 refuses persist
  AC2  densify Phase-5 + steal TypeLinear proof stamps use the same
       join mid under production/Full (Soft may keep defuse observe)
  AC3  Soft/Off: existing Soft observe / zero-extra faces retained
  AC4  No new query key; no second audit bus; no test_issue_3778.cpp;
       no docs/design/; linter after #3653

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    tma = _read("src/compiler/typed_mutation_audit.h")
    test = _read("tests/compiler/test_audit_mutation_id_unify.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    qr = _read("src/compiler/evaluator_primitives_query_reflect.cpp")

    pre = mb.find("outermost-pre-persist")
    # Prefer the call site after pre-persist (#3780 may add an early
    # extern/decl of aura_outermost_success_persist_occurrence).
    persist = -1
    if pre >= 0:
        persist = mb.find("aura_outermost_success_persist_occurrence(ev_", pre)
    if persist < 0:
        persist = mb.find("aura_outermost_success_persist_occurrence(ev_")
    densify = mb.find("densify_stamp_mid_3778")
    steal = efm.find("steal_stamp_mid_3778")

    must("Issue #3778", "AC1 cite", mb)
    must("kOccurrenceMidJoinSsotIssue = 3778", "AC1 stamp", tma)
    if pre < 0 or persist < 0:
        fails.append("AC1: pre-persist / persist sites missing")
    else:
        pre_win = mb[pre - 400 : persist]
        must("join_audit_and_se_mid(0)", "AC1 pre-persist join", pre_win)
        must("stk.back().audit_mid", "AC1 pre-persist cp.audit_mid", pre_win)
        must_not(
            "defuse_version_.load(std::memory_order_relaxed);\n        (void)ev_->run_typed_mutation_invariant_audit",
            "AC1 no defuse mid_audit",
            pre_win,
        )
        persist_win = mb[persist - 4000 : persist + 200]  # #3780 WAL gate sits between mid SSOT and persist call
        must("hard_3778", "AC1 mid==0 refuse gate", persist_win)
        must("join_audit_and_se_mid(0)", "AC1 persist join", persist_win)
        must("session_mid_at_enter_", "AC1 session mid", persist_win)
        # Production refuse must not fall through to defuse as join mid.
        must("mid == 0", "AC1 mid==0 refuse", persist_win)
        must("kTypeLinearProofOutcomeReject", "AC1 refuse reject face", persist_win)

    must("densify_stamp_mid_3778", "AC2 densify stamp", mb)
    must("steal_stamp_mid_3778", "AC2 steal stamp", efm)
    if densify >= 0:
        # densify_join_mid_3778 / densify_hard sit just above densify_stamp_mid_3778
        dwin = mb[max(0, densify - 500) : densify + 1200]
        must("densify_hard_3778", "AC2 densify hard face", dwin)
        must("session_mid_at_enter_", "AC2 densify session", dwin)
        must("join_audit_and_se_mid", "AC2 densify join", dwin)
    if steal >= 0:
        swin = efm[steal - 250 : steal + 500]
        must("steal_hard_3778", "AC2 steal hard face", swin)
        must("join_audit_and_se_mid(0)", "AC2 steal join", swin)

    # Soft: Soft observe still allowed when join mid is 0 (persist) /
    # densify Soft still may load defuse for observe stamp.
    must("!hard_3778 && mid == 0", "AC3 Soft persist observe", mb)
    must("densify_hard_3778 ? densify_join_mid_3778", "AC3 Soft densify defuse", mb)
    must("steal_hard_3778 ? typed_audit::join_audit_and_se_mid(0)", "AC3 Soft steal defuse", efm)

    must("check_outermost_persist_audit_order_3653", "AC4 prev linter", build)
    must("check_occurrence_mid_join_ssot_3778", "AC4 build.py", build)
    prev = build.find("check_outermost_persist_audit_order_3653")
    ours = build.find("check_occurrence_mid_join_ssot_3778")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3653")
    must("#3778 AC1: occurrence persist mid joins SSOT", "AC4 test cite", test)
    must_not("schema-3778", "AC4 no new query key", q + qr)
    if _read("tests/compiler/test_issue_3778.cpp"):
        fails.append("AC4: test_issue_3778.cpp present")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3778-*")):
            fails.append(f"AC4: docs/design/{f.name} present")

    if fails:
        print(f"FAIL #3778 occurrence_mid_join_ssot ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3778 occurrence_mid_join_ssot")
    return 0


if __name__ == "__main__":
    sys.exit(main())
