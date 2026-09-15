#!/usr/bin/env python3
"""Issue #3811: Evaluator::live_compact Soft|Force Densify restamp (Agent entry).

Explicit Agent / (arena:live-compact) Soft|Force path bumped CompilerMetrics
on invalidates_pins but historically did not call
unified_restamp_after_boundary(Densify). GC Soft (#3677/#3742) and boundary
Soft probe (#3809) already restamp; Agent Soft mid-session did not.

#3811 mirrors that belt inside Evaluator::live_compact: snapshot gen_at_entry;
if Soft|Force advanced gen / invalidated / remapped pins, Densify restamp
before return. Soft stays non-Moving (no Moving window publish). Soft
soft-gated no-op → zero extra restamp. Moving path unchanged (relocate hook).

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

    ixx = _read("src/compiler/evaluator.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    gc = _read("src/compiler/evaluator_gc.cpp")
    test = _read("tests/serve/test_gc_compact_sweep_batch.cpp")
    build = _read("build.py")
    gf = _read("scripts/coverage/simple_check_grandfather.txt")

    # Window includes preamble comments above the signature (#3811 cite block).
    live = ixx.find("Issue #2004: Evaluator-level live_compact")
    if live == -1:
        live = ixx.find("live_compact(aura::ast::LiveCompactMode mode =")
    end = ixx.find("void probe_arena_auto_policy_on_fiber_transition", live if live != -1 else 0)
    win = ixx[live:end] if live != -1 and end != -1 and end > live else ""

    # AC1: Soft that sets invalidates-pins restamps Densify before return
    must("Issue #3811", "AC1 cite live_compact", win)
    must("gen_at_entry", "AC1 gen snapshot", win)
    must("LiveCompactMode::Soft", "AC1 Soft mode", win)
    must("LiveCompactMode::Force", "AC1 Force mode", win)
    must("lc.invalidates_pins || lc.remapped_pins > 0", "AC1 invalidates/remap arm", win)
    must("lc.new_gen != 0 && lc.new_gen != gen_at_entry", "AC1 new_gen arm", win)
    must(
        "unified_restamp_after_boundary(UnifiedRestampSite::Densify)",
        "AC1 Densify restamp",
        win,
    )
    must("Issue #3811", "AC1 obs primitive cite", obs)

    # Dual-track retained on GC Soft + boundary Soft
    must("Issue #3677", "AC1 GC Soft cite #3677", gc)
    must("Issue #3742", "AC1 GC Soft cite #3742", gc)
    must("Issue #3809", "AC1 boundary Soft cite #3809", ixx)

    # AC2: soft-gated no-op still zero extra
    must("soft-gated", "AC2 soft-gated comment", win)
    must("3811 AC2", "AC2 test marker", test)

    # AC3: Moving unchanged; Soft does not publish Moving window
    must("no publish_last_moving_densify_window", "AC3 Soft non-Moving comment", win)
    if "publish_last_moving_densify_window(" in win:
        fails.append("AC3: Soft|Force live_compact must not call publish_last_moving_densify_window(")
    must("Moving path unchanged", "AC3 Moving unchanged comment", win)
    must("3811 AC3", "AC3 test marker", test)

    # AC4 CI soak
    must("run_3811_agent_soft_densify_restamp", "AC4 test harness", test)
    must("3811 AC4", "AC4 apply/query marker", test)
    must("apply_closure", "AC4 apply_closure", test[test.find("run_3811_agent_soft_densify_restamp") :])
    must("query:stable-ref-stats", "AC4 query:stable-ref", test)

    must("check_agent_soft_densify_restamp_3811", "AC wire build.py", build)
    must("Issue #3811", "AC build cite", build)
    must("check_agent_soft_densify_restamp_3811.py", "AC grandfather", gf)

    for rel in (
        "tests/serve/test_issue_3811.cpp",
        "tests/compiler/test_issue_3811.cpp",
        "tests/issues/test_issue_3811.cpp",
    ):
        if _read(rel):
            fails.append(f"forbidden invent: {rel} exists")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("3811-*.md"):
            fails.append(f"forbidden design doc: {f.relative_to(ROOT)}")

    if fails:
        print(f"Issue #3811 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3811 Agent Soft|Force Densify restamp — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
