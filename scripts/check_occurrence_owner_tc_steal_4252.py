#!/usr/bin/env python3
"""Issue #4252: steal/densify drops the proof face but the Occurrence persist
clear was keyed only off the TLS commit TypeChecker (or stamp last-look TC).

invalidate_fast_path_before_steal_densify_restamp always clears the
process-global TypeLinearCommitProof face (would_allow / linear_ok / stamper)
and advances invalidate_gen, but the helper clear_occurrence_persist_on_
steal_densify_success_ resolved its target from
aura_typed_audit_current_commit_type_checker() / g_tls_stamp_last_look_tc
only. During a worker steal / densify restamp the thief's TLS is not the
victim's (fiber handoff), and the TLS commit TC may be null, so the owner's
Occurrence persist snapshot survived the face drop; a later rehydrate on the
owner could copy the pre-steal narrowing over a rejected proof.

Fix shape (no second model): thread the restamping Evaluator (opaque void*
ABI - the header must stay TypeChecker-free) through the clear, so under
production/Full the OWNER's commit TypeChecker buffer is dropped in the same
function that drops the face. The zero-arg form keeps the TLS fallback for
evaluator-less callers; a peer's buffer is never cleared. No new query key,
no restamp-green here (#2938).

Contract:
  AC1  header declares the evaluator-aware invalidate overload + forwards the
       zero-arg form; the core keeps the #3416 stamper/gen drop and hard gate
  AC2  clear helper takes void* ev; ev -> owner clear via the #3482 Evaluator
       ABI; nullptr -> existing TLS fallback intact
  AC3  runtime call sites pass the evaluator (fiber this / densify ev_ /
       note_densify_entry_under_live_mutation ev)
  AC4  #3482 / #3416 / #3063 surfaces preserved
  AC5  Soft/Off early return unchanged; no new query key / second model
  AC6  runtime ACs in the existing persist-rehydrate suite; no test_issue_4252
       / docs/design/4252-*
  AC7  build.py wiring + root_check_allowlist.txt entry

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: evaluator-aware overload + zero-arg fallback forwarder ──────
    must("Issue #4252", "AC1 header cites the issue", tma)
    must(
        "invalidate_fast_path_before_steal_densify_restamp(void* ev) noexcept",
        "AC1 evaluator-aware overload declared/defined",
        tma,
    )
    zpos = tma.find("bool invalidate_fast_path_before_steal_densify_restamp() noexcept")
    zwin = tma[zpos : zpos + 1600] if zpos >= 0 else ""
    must("invalidate_fast_path_before_steal_densify_restamp(nullptr)", "AC1 zero-arg form forwards to the core", zwin)
    must("production_defaults_active() || get_strategy() == AuditStrategy::Full", "AC1 hard gate kept", zwin)
    must("g_last_proof_stamper_eval.store(0", "AC1 #3416 stamper drop kept", zwin)
    must("g_rehydrate_miss_invalidate_gen.fetch_add", "AC1 #3416 gen drop kept", zwin)
    must("clear_occurrence_persist_on_steal_densify_success_(ev)", "AC1 core forwards the eval to the clear", zwin)
    must_not("publish_last_proof_face(true", "AC1 no restamp-green", zwin)

    # ── AC2: helper keys the clear off the owner eval ───────────────────
    hpos = tma.find("inline void clear_occurrence_persist_on_steal_densify_success_(void* ev) noexcept {")
    hwin = tma[hpos : hpos + 1200] if hpos >= 0 else ""
    must("if (ev) {", "AC2 owner branch present", hwin)
    must("aura_clear_occurrence_persist_buffer(ev)", "AC2 owner clear via #3482 ABI", hwin)
    must("return;", "AC2 owner branch returns before TLS", hwin)
    must("aura_typed_audit_current_commit_type_checker()", "AC2 TLS fallback kept", hwin)
    must("clear_occurrence_persist_buffer(tc)", "AC2 TLS fallback reuses #3170", hwin)

    # ── AC3: call sites pass their evaluator ────────────────────────────
    must(
        "invalidate_fast_path_before_steal_densify_restamp(this)",
        "AC3 fiber steal/densify site passes this",
        efm.rstrip(),
    )
    must("invalidate_fast_path_before_steal_densify_restamp(ev_)", "AC3 densify relocate site passes ev_", mb)
    ndpos = tma.find("note_densify_entry_under_live_mutation(void* ev = nullptr)")
    ndwin = tma[ndpos : ndpos + 1400] if ndpos >= 0 else ""
    must("invalidate_fast_path_before_steal_densify_restamp(ev)", "AC3 note_densify forwards its evaluator", ndwin)

    # ── AC4: prior steal/densify surfaces preserved ─────────────────────
    must("aura_clear_occurrence_persist_buffer(this)", "AC4 #3482 fiber clear kept", efm)
    must("aura_clear_occurrence_persist_buffer(ev_)", "AC4 #3482 densify clear kept", mb)
    must("Issue #3482", "AC4 #3482 cite kept", tma)
    must("Issue #3063", "AC4 #3063 cite kept", tma)

    # ── AC5: Soft/Off unchanged; no new query key -----------------------
    must("if (!hard)", "AC5 hard gate early-out kept", zwin)
    must_not("schema-4252", "AC5 no schema-4252", tma)
    must_not("g_4252_", "AC5 no g_4252_* counter", tma)
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp") + _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    must_not("query:steal-densify-owner-tc", "AC5 no new query key", q)

    # ── AC6: runtime ACs dispatched in the existing suite ───────────────
    for fn in (
        "ac4252_1_owner_tc_cleared_when_tls_null",
        "ac4252_2_peer_tls_tc_not_cleared",
        "ac4252_3_soft_no_extra_and_source_cite",
    ):
        must(fn, "AC6 AC defined", t)
        must(fn + "();", "AC6 AC dispatched", t)
    if (ROOT / "tests" / "compiler" / "test_issue_4252.cpp").is_file() or (
        ROOT / "tests" / "issues" / "test_issue_4252.cpp"
    ).is_file():
        fails.append("AC6: test_issue_4252.cpp must not exist (#81934)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("4252-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    # ── AC7: build.py + allowlist wiring ────────────────────────────────
    must("check_occurrence_owner_tc_steal_4252.py", "AC7 build.py wires the linter", build)
    must("check_occurrence_owner_tc_steal_4252.py", "AC7 root allowlist entry", allow)

    if fails:
        print("FAIL #4252 occurrence_owner_tc_steal:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #4252 occurrence_owner_tc_steal: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
