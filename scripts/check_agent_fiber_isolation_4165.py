#!/usr/bin/env python3
"""Issue #4165: same-tenant multi-Agent code-as-memory had no Agent-scoped
isolation beyond Guard + epoch (P1).

Hole: production Restricted+MT hard-rejects cross-tenant and compares
fiber/cow on QueryResult / StableNodeRef freshness — but two Agents with the
SAME tenant (and often fiber 0) share one workspace_flat_ authority. Agent
entry that does not mint a fiber leaves aura_fiber_current_id() == 0, so
every export stamped fiber_id 0 and every resolve passed current 0: the
InvalidFiber check (`current_fiber_id != 0 && m.fiber_id != current_fiber_id`)
was permanently skipped. Agent A's held QueryResult resolved under Agent B
with no Agent-scoped deny; deploying multi-Agent loops on one Evaluator
required operational fiber/tenant partitioning the EDSL did not enforce.

Fix shape (reuse the existing fiber stamp path; no second query/mutate API):
  - Evaluator::agent_scoped_fiber_id() (evaluator_security.cpp): resolution
    order is existing SSOTs only — explicit caller fiber > #2151
    effect_fiber_id_or override > live aura_fiber_current_id(); still 0
    under the production face → a lazily-minted stable per-Evaluator id in
    the 0x41650000 Agent band (registry fiber ids are small pool indices).
    Soft / Off keep the legacy 0 stamp (no new soft face).
  - stamp_stable_ref + make_stamped_safe_ref route the fiber through the
    resolution, so every export (incl. stamp_query_stable_ref_export and
    stamp_query_result_full_provenance) carries the Agent-scoped stamp.
  - Resolve sites pass the same resolution: resolve_mutate_node_arg
    (mutate.cpp), query:result-fresh? / query:result-matches
    (query_workspace.cpp), query:stable-ref-provenance (query.cpp).
  - The hard InvalidFiber face is kept verbatim (no weakening).
  - Deploy contract: one fiber (or tenant) per Agent — the mint only backs
    fiberless production entry.

Contract (one row per AC):
  AC1  agent_scoped_fiber_id defined in evaluator_security.cpp with the
       Agent band + per-process mint sequence; stamp_stable_ref and
       make_stamped_safe_ref route the fiber through it (explicit fiber
       wins in make_stamped_safe_ref)
  AC2  mint is production-gated (production_defaults_active) with the Soft
       legacy 0 return; per-Evaluator cache member in evaluator.ixx
  AC3  resolve sites wired: mutate.cpp resolve_mutate_node_arg,
       query:result-fresh? + query:result-matches (>=2 in query_workspace),
       query:stable-ref-provenance (query.cpp)
  AC4  hard InvalidFiber face kept verbatim in query_result_decode.hh (hard
       and soft arms) and InvalidFiber = 3 unchanged in workspace_epoch.hh
  AC5  runtime ACs dispatched in both suites (fiber_cow ac4165_1..5 +
       tenant ac4165_agent_fiber_isolation); no tests/**/test_issue_4165.cpp;
       no docs/design/4165-*
  AC6  build.py wiring + root_check_allowlist.txt entry

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

    def absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    def window(src: str, begin: str, end: str, label: str, width: int = 5200) -> str:
        b = src.find(begin)
        if b == -1:
            fails.append(f"{label}: anchor {begin!r} missing")
            return ""
        e = src.find(end, b + len(begin))
        return src[b : (e if e != -1 else b + width)]

    sec = _read("src/compiler/evaluator_security.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    qprov = _read("src/compiler/evaluator_primitives_query.cpp")
    dec = _read("src/compiler/query_result_decode.hh")
    epoch = _read("src/core/workspace_epoch.hh")
    cow_test = _read("tests/serve/test_stable_ref_provenance_fiber_cow.cpp")
    tenant_test = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: Agent-scoped resolution + stamp-path routing ────────────────
    must(
        "std::uint32_t Evaluator::agent_scoped_fiber_id() const noexcept",
        "AC1 resolution defined in the security TU",
        sec,
    )
    must("kAgentFiberStampBand = 0x4165'0000u", "AC1 Agent band constant", sec)
    must("g_agent_fiber_stamp_seq", "AC1 per-process mint sequence", sec)
    ssr = window(
        sec, "void Evaluator::stamp_stable_ref", "g_isolation_capture_stamp_local_total_atomic", "AC1 stamp_stable_ref"
    )
    must("Issue #4165", "AC1 stamp_stable_ref cites the issue", ssr)
    must("const auto fiber = agent_scoped_fiber_id();", "AC1 stamp routes the resolution", ssr)
    msr = window(sec, "Evaluator::make_stamped_safe_ref", "stamp_stable_ref(ref);", "AC1 make_stamped_safe_ref")
    must("fiber_id != 0 ? fiber_id : agent_scoped_fiber_id()", "AC1 explicit fiber wins, else Agent-scoped", msr)

    # ── AC2: production-gated mint + per-Evaluator cache ─────────────────
    defin = window(
        sec,
        "std::uint32_t Evaluator::agent_scoped_fiber_id()",
        "void Evaluator::stamp_ref_tenant",
        "AC2 resolution body",
    )
    must("production_defaults_active()", "AC2 mint production-gated", defin)
    must("return 0;", "AC2 Soft legacy 0 return", defin)
    must("effect_fiber_id_or", "AC2 #2151 override honored", defin)
    must("aura_fiber_current_id", "AC2 live fiber honored", defin)
    must("agent_fiber_id_ == 0", "AC2 lazy per-Evaluator mint", defin)
    must("agent_scoped_fiber_id() const noexcept", "AC2 evaluator.ixx declaration", ixx)
    must("mutable std::uint32_t agent_fiber_id_ = 0", "AC2 per-Evaluator cache member", ixx)

    # ── AC3: resolve sites wired ─────────────────────────────────────────
    mres = window(mut, "auto resolve_mutate_node_arg", "is_int(arg))", "AC3 mutate resolve")
    must("ev.agent_scoped_fiber_id()", "AC3 resolve_mutate_node_arg Agent-scoped", mres)
    hits = qws.count("ev.agent_scoped_fiber_id()")
    if hits < 2:
        fails.append(f"AC3 query_workspace resolve sites: expected >=2, found {hits}")
    fresh = window(qws, 'add("query:result-fresh?"', 'add("query:result-matches"', "AC3 result-fresh?")
    must("Issue #4165", "AC3 result-fresh? cites the issue", fresh)
    matches = window(qws, 'add("query:result-matches"', 'add("query:query-epoch-stats"', "AC3 result-matches")
    must("Issue #4165", "AC3 result-matches cites the issue", matches)
    must("ev.agent_scoped_fiber_id()", "AC3 stable-ref-provenance Agent-scoped", qprov)

    # ── AC4: hard InvalidFiber face kept verbatim ────────────────────────
    must("if (current_fiber_id != 0 && m.fiber_id != current_fiber_id) {", "AC4 hard InvalidFiber arm kept", dec)
    must(
        "if (current_fiber_id != 0 && m.fiber_id != 0 && m.fiber_id != current_fiber_id) {",
        "AC4 soft InvalidFiber arm kept",
        dec,
    )
    must("InvalidFiber = 3", "AC4 InvalidFiber enum value unchanged", epoch)

    # ── AC5: runtime ACs dispatched; no invented test / docs ─────────────
    for fn in (
        "ac4165_1_prod_fiberless_entry_stamps_agent_band",
        "ac4165_2_same_agent_requery_fresh",
        "ac4165_3_cross_agent_same_tenant_deny",
        "ac4165_4_source_cite",
        "ac4165_5_no_docs_linter_wired",
    ):
        must(fn, "AC5 fiber_cow AC defined", cow_test)
        must(fn + "();", "AC5 fiber_cow AC dispatched", cow_test)
    must("0x41650000", "AC5 Agent-band runtime assertion", cow_test)
    must("static void ac4165_agent_fiber_isolation()", "AC5 tenant AC defined", tenant_test)
    must("ac4165_agent_fiber_isolation();", "AC5 tenant AC dispatched", tenant_test)
    if (ROOT / "tests/core/test_issue_4165.cpp").exists():
        fails.append("AC5: tests/core/test_issue_4165.cpp must not exist (#81934)")
    if (ROOT / "tests/serve/test_issue_4165.cpp").exists():
        fails.append("AC5: tests/serve/test_issue_4165.cpp must not exist (#81934)")
    if any((ROOT / "docs" / "design").glob("4165-*")) if (ROOT / "docs" / "design").is_dir() else False:
        fails.append("AC5: no docs/design/4165-*")

    # ── AC6: build.py + allowlist wiring ─────────────────────────────────
    must("check_agent_fiber_isolation_4165.py", "AC6 build.py wires the linter", build)
    must("check_agent_fiber_isolation_4165.py", "AC6 root allowlist entry", allow)

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("check_agent_fiber_isolation_4165: OK (6 AC rows satisfied)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
