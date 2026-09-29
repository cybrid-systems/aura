#!/usr/bin/env python3
"""Issue #4156 source-cite gate: AdaptiveThrPolicy (#2248) dual-track closure.

The pre-#4156 AdaptiveThrPolicy face RAISED current_thr under
correctness-risk fallbacks (MapInconsistent / DesyncForceFull) while the
production peel path (get_partial_relower_threshold +
decide_workload_adaptive_partial_relower / consult_workload_adaptive_)
never read it — a dual-track where Agents trusting the hardcoded
adaptive-thr-wired=1 face and driving set_partial_relower_threshold from
adaptive-thr-current would WIDEN partial exactly when correctness risk
says prefer full sooner (mutual injury).

Fix shape (issue option 1 — wire with inverted polarity): correctness
risk LOWERS the face (floored at 0.25x base) and the decision core
(decide_workload_adaptive_partial_relower) clamps the effective partial
thr to it, so every peel entry — should_partial_relower_workload*,
consult_workload_adaptive_partial_ — shares one wired threshold. Clean
windows recover toward base (never above). AURA_ADAPTIVE_THR=0 keeps the
zero-cost Soft/Off face: note early-returns, clamp skipped.

ACs:
  AC1  inverted polarity in ir_cache_pure.ixx: the risk branch LOWERS
       current_thr (kMinRatioBp floor, "narrowed < min_thr" clamp); the
       pre-#4156 raise arithmetic (kMaxRatioBp) is gone; block doc cites
       #4156 and states Agents cannot widen partial via this face.
  AC2  clean-window recovery: Ok recovers one step toward base and is
       capped at base ("raised > p.base_partial_cost_thr" arm).
  AC3  decision-core wiring: decide_workload_adaptive_partial_relower
       reads current_adaptive_partial_thr() under an
       adaptive_thr_frozen() guard, clamps the effective thr, and sets
       kAdaptiveReasonAdaptiveThrRisk (bit 1u << 10).
  AC4  zero-cost Soft/Off: note_relower_fallback_for_adaptive
       early-returns under the AURA_ADAPTIVE_THR freeze; the decide
       clamp is behind the same freeze.
  AC5  face truth + query stability: adaptive-thr-wired stays make_int(1)
       with a #4156 wiring comment; adaptive-thr-current / schema-2248
       keys intact; observability_metrics.h comments state the inverted
       polarity (decays on bad reasons, raises on clean recovery).
  AC6  consult inheritance documented: service.ixx
       consult_workload_adaptive_partial_ cites #4156 (clamp applied
       inside decide_workload_adaptive_partial_relower).
  AC7  test home + registry: test_adaptive_partial_relower_threshold.cpp
       hosts ac4156_adaptive_thr_feeds_peel_inverted with the
       prefer-full-under-risk runtime CHECK; no tests/compiler/
       test_issue_4156.cpp; no docs/design/4156-*; build.py registers
       this linter and scripts/coverage/root_check_allowlist.txt lists
       check_adaptive_thr_4156.py.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IRC = ROOT / "src" / "compiler" / "ir_cache_pure.ixx"
MET = ROOT / "src" / "compiler" / "observability_metrics.h"
OBS = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
SVC = ROOT / "src" / "compiler" / "service.ixx"
TST = ROOT / "tests" / "compiler" / "test_adaptive_partial_relower_threshold.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

checks: list[tuple[bool, str]] = []


def must(cond: bool, label: str) -> None:
    checks.append((bool(cond), label))


def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i + len(start))
    return text[i:j] if j > 0 else text[i:]


def main() -> int:
    irc = IRC.read_text(encoding="utf-8")
    met = MET.read_text(encoding="utf-8")
    obs = OBS.read_text(encoding="utf-8")
    svc = SVC.read_text(encoding="utf-8")
    tst = TST.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    allow = ALLOW.read_text(encoding="utf-8")

    # ── AC1: inverted polarity in the policy mutator ──
    note = _between(
        irc, "inline void note_relower_fallback_for_adaptive", 'extern "C" void aura_note_relower_fallback_for_adaptive'
    )
    must("#4156 inverted polarity" in note, "AC1: note mutator cites #4156 inverted polarity")
    must("kMinRatioBp" in note, "AC1: risk branch floors at kMinRatioBp")
    must("p.current_thr = narrowed < min_thr" in note, "AC1: risk branch lowers current_thr")
    must("kMaxRatioBp" not in irc, "AC1: pre-#4156 raise arithmetic (kMaxRatioBp) removed")
    block = _between(irc, "── Issue #2248:", "struct AdaptiveThrPolicy")
    must("#4156 inverted polarity + wiring" in block, "AC1: block doc cites #4156 wiring")
    must(
        "Agents cannot widen partial" in block or "cannot widen partial via this face" in block,
        "AC1: doc states Agents cannot widen partial via this face",
    )
    struct = _between(irc, "struct AdaptiveThrPolicy {", "inline AdaptiveThrPolicy&")
    must(
        "#4156: lowered under bad reasons, <= base always" in struct,
        "AC1: current_thr field documents <= base invariant",
    )

    # ── AC2: clean-window recovery capped at base ──
    must("if (p.current_thr < p.base_partial_cost_thr)" in note, "AC2: recovery only runs below base")
    must("raised > p.base_partial_cost_thr" in note, "AC2: recovery capped at base (never above — no Agent widen)")

    # ── AC3: decision-core wiring ──
    decide = _between(
        irc,
        "[[nodiscard]] inline AdaptiveRelowerDecision decide_workload_adaptive_partial_relower",
        "inline std::atomic<double>& g_shape_stability_ratio_atomic",
    )
    must("Issue #4156: close the #2248 dual-track" in decide, "AC3: decide core cites #4156 dual-track closure")
    must("current_adaptive_partial_thr()" in decide, "AC3: decide core reads the adaptive face")
    must("if (!adaptive_thr_frozen())" in decide, "AC3: clamp guarded by the freeze")
    must("kAdaptiveReasonAdaptiveThrRisk" in decide, "AC3: clamp sets the risk reason bit")
    must("kAdaptiveReasonAdaptiveThrRisk = 1u << 10" in irc, "AC3: risk reason bit declared (1u << 10)")

    # ── AC4: zero-cost Soft/Off ──
    must("if (adaptive_thr_frozen())" in note, "AC4: note mutator early-returns when frozen")
    must("AURA_ADAPTIVE_THR" in irc, "AC4: env override AURA_ADAPTIVE_THR intact")

    # ── AC5: face truth + query stability ──
    wired = _between(obs, '"adaptive-thr-current"', '"schema-2248"')
    must('"adaptive-thr-wired", make_int(1)' in wired, "AC5: adaptive-thr-wired stays 1")
    must("Issue #4156: genuinely wired" in wired, "AC5: wired key cites the #4156 wiring")
    must(
        '"adaptive-thr-current"' in obs and '"schema-2248"' in obs,
        "AC5: adaptive-thr-current + schema-2248 query keys intact",
    )
    must(
        "#4156 inverted" in met and "bad-reason window lowered thr" in met,
        "AC5: metrics comments state the inverted polarity",
    )
    must("adaptive_thr_current{800}" in met, "AC5: adaptive_thr_current field intact")

    # ── AC6: consult inheritance documented ──
    consult = _between(
        svc, "// Issue #2212: Shape bit widens partial window", "metrics_.partial_relower_threshold_used.store"
    )
    must(
        "Issue #4156: the #2248 AdaptiveThrPolicy risk clamp" in consult, "AC6: consult cites the inherited #4156 clamp"
    )

    # ── AC7: test home + registry ──
    must("ac4156_adaptive_thr_feeds_peel_inverted" in tst, "AC7: ac4156 AC function present")
    must(
        "4156 AC1: peel prefers full under correctness risk" in tst, "AC7: prefer-full-under-risk runtime CHECK present"
    )
    must("ac4156_adaptive_thr_feeds_peel_inverted();" in tst, "AC7: dispatcher invokes the runtime door")
    must("check_adaptive_thr_4156.py" in build, "AC7: build.py registers this linter")
    must("check_adaptive_thr_4156.py" in allow, "AC7: root_check_allowlist.txt lists this linter")
    must(
        not (ROOT / "tests" / "compiler" / "test_issue_4156.cpp").exists(), "AC7: no tests/compiler/test_issue_4156.cpp"
    )
    must(not list((ROOT / "docs" / "design").glob("4156-*")), "AC7: no docs/design/4156-* markdown")

    failed = 0
    for ok, label in checks:
        if ok:
            print(f"  ok   {label}")
        else:
            failed += 1
            print(f"  FAIL {label}")
    print(f"check_adaptive_thr_4156: {len(checks) - failed}/{len(checks)} rows green")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
