#!/usr/bin/env python3
"""Issue #4235: agent_scoped_fiber_id is per-Evaluator, not per-Agent —
same-Eval multi-Agent leak (P1 security residual after #4165).

Hole: #4165 minted ONE Agent-band id per Evaluator (agent_fiber_id_ single
cache slot). Two (or N) Agents sharing one Evaluator fiberless under the
production face all resolved agent_scoped_fiber_id() to the SAME mint, so
same-tenant code-as-memory held refs (QueryResult / StableNodeRef) passed
the InvalidFiber freshness check against each other: Agent A's export
resolved fresh under Agent B (`current != 0 && current != stamped` was
false — same id on both sides). The #4165 deploy contract ("one fiber (or
tenant) per Agent — the mint only backs fiberless production entry") was
not enforced by the EDSL for the common same-Evaluator multi-Agent loop.

Fix shape (no second model; the hard InvalidFiber face is untouched):
  - The mint is per-Agent: keyed on the #1419 agent fingerprint
    (current_agent_fingerprint) via a fixed 32-slot lock-free table
    (agent_mint_slots_) on the Evaluator — slot word packs the fingerprint
    key (high 32) and the minted Agent-band id (low 32); one single-word
    CAS per (Evaluator, fingerprint) claim, mints draw from the shared
    #4165 per-process sequence so two Agents can never observe one id.
  - Fingerprint 0 (no identity installed — the Evaluator's single implicit
    agent) keeps #4165's per-Evaluator agent_fiber_id_ fallback verbatim.
  - Resolution order unchanged: explicit caller fiber > #2151
    effect_fiber_id_or override > live aura_fiber_current_id() > mint;
    production gate and Soft/Off legacy 0 stamp unchanged.
  - Overflow beyond 32 identified Agents on one Evaluator degrades to the
    per-Evaluator mint — fail-closed (a shared mint can only widen
    InvalidFiber denies, never admit a foreign resolve).
  - Stamp/resolve wiring and the hard InvalidFiber face stay verbatim.

Contract (one row per AC):
  AC1  agent_scoped_fiber_id keys the mint on the #1419 agent fingerprint
       via the lock-free agent_mint_slots_ table (hit-scan + CAS claim +
       shared sequence), preserving the #4165 resolution order, production
       gate, Soft legacy 0, and the per-Evaluator fallback arm
  AC2  evaluator.ixx declares the per-Agent table (kAgentMintSlotCount = 32,
       agent_mint_slots_) next to the kept per-Evaluator agent_fiber_id_
  AC3  downstream wiring unchanged: stamp_stable_ref routes
       agent_scoped_fiber_id, make_stamped_safe_ref explicit-wins, the 3
       resolve sites pass ev.agent_scoped_fiber_id(), and the hard+soft
       InvalidFiber arms plus InvalidFiber = 3 are verbatim
  AC4  runtime ACs dispatched in test_stable_ref_provenance_fiber_cow.cpp
       (ac4235_1..5); no tests/**/test_issue_4235.cpp; no docs/design/4235-*
  AC5  build.py wiring + root_check_allowlist.txt entry

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

    def window(src: str, begin: str, end: str, label: str, width: int = 7200) -> str:
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
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: per-Agent keyed mint in the security TU ─────────────────────
    defin = window(
        sec,
        "std::uint32_t Evaluator::agent_scoped_fiber_id()",
        "void Evaluator::stamp_ref_tenant",
        "AC1 resolution body",
    )
    must("Issue #4235", "AC1 resolution cites the issue", defin)
    must("current_agent_fingerprint()", "AC1 mint keyed on the #1419 fingerprint", defin)
    must("agent_mint_slots_", "AC1 per-Agent table consulted", defin)
    must("(w >> 32) == key32", "AC1 slot word: fingerprint key in the high half", defin)
    must("compare_exchange_strong", "AC1 single-word CAS claim", defin)
    must("kAgentFiberStampBand", "AC1 mint stays in the Agent band", defin)
    must("g_agent_fiber_stamp_seq", "AC1 mints draw from the shared #4165 sequence", defin)
    must("effect_fiber_id_or", "AC1 #2151 override honored first", defin)
    must("aura_fiber_current_id", "AC1 live fiber honored", defin)
    must("production_defaults_active()", "AC1 mint production-gated", defin)
    must("return 0;", "AC1 Soft legacy 0 return kept", defin)
    must("agent_fiber_id_ == 0", "AC1 #4165 per-Evaluator fallback arm kept", defin)

    # ── AC2: per-Agent table declared next to the kept fallback ─────────
    must("kAgentMintSlotCount = 32", "AC2 table slot count constant", ixx)
    must("mutable std::atomic<std::uint64_t> agent_mint_slots_", "AC2 table member", ixx)
    must("mutable std::uint32_t agent_fiber_id_ = 0", "AC2 per-Evaluator fallback kept", ixx)
    must("agent_scoped_fiber_id() const noexcept", "AC2 declaration unchanged", ixx)

    # ── AC3: stamp/resolve wiring + hard InvalidFiber face verbatim ──────
    ssr = window(
        sec, "void Evaluator::stamp_stable_ref", "g_isolation_capture_stamp_local_total_atomic", "AC3 stamp_stable_ref"
    )
    must("const auto fiber = agent_scoped_fiber_id();", "AC3 stamp routes the per-Agent resolution", ssr)
    msr = window(sec, "Evaluator::make_stamped_safe_ref", "stamp_stable_ref(ref);", "AC3 make_stamped_safe_ref")
    must("fiber_id != 0 ? fiber_id : agent_scoped_fiber_id()", "AC3 explicit fiber wins, else per-Agent", msr)
    mres = window(mut, "auto resolve_mutate_node_arg", "is_int(arg))", "AC3 mutate resolve")
    must("ev.agent_scoped_fiber_id()", "AC3 resolve_mutate_node_arg rides the resolution", mres)
    hits = qws.count("ev.agent_scoped_fiber_id()")
    if hits < 2:
        fails.append(f"AC3 query_workspace resolve sites: expected >=2, found {hits}")
    must("ev.agent_scoped_fiber_id()", "AC3 stable-ref-provenance rides the resolution", qprov)
    must("if (current_fiber_id != 0 && m.fiber_id != current_fiber_id) {", "AC3 hard InvalidFiber arm verbatim", dec)
    must(
        "if (current_fiber_id != 0 && m.fiber_id != 0 && m.fiber_id != current_fiber_id) {",
        "AC3 soft InvalidFiber arm verbatim",
        dec,
    )
    must("InvalidFiber = 3", "AC3 InvalidFiber enum value unchanged", epoch)

    # ── AC4: runtime ACs dispatched; no invented test / docs ─────────────
    for fn in (
        "ac4235_1_per_agent_distinct_mints_one_evaluator",
        "ac4235_2_cross_agent_held_result_deny_same_evaluator",
        "ac4235_3_unidentified_agent_keeps_per_evaluator_mint",
        "ac4235_4_override_beats_agent_mint",
        "ac4235_5_source_cite",
    ):
        must(fn, "AC4 fiber_cow AC defined", cow_test)
        must(fn + "();", "AC4 fiber_cow AC dispatched", cow_test)
    if (ROOT / "tests/core/test_issue_4235.cpp").exists():
        fails.append("AC4: tests/core/test_issue_4235.cpp must not exist (#81934)")
    if (ROOT / "tests/serve/test_issue_4235.cpp").exists():
        fails.append("AC4: tests/serve/test_issue_4235.cpp must not exist (#81934)")
    if any((ROOT / "docs" / "design").glob("4235-*")) if (ROOT / "docs" / "design").is_dir() else False:
        fails.append("AC4: no docs/design/4235-*")

    # ── AC5: build.py + allowlist wiring ─────────────────────────────────
    must("check_agent_mint_per_agent_4235.py", "AC5 build.py wires the linter", build)
    must("check_agent_mint_per_agent_4235.py", "AC5 root allowlist entry", allow)

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("check_agent_mint_per_agent_4235: OK (5 AC rows satisfied)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
