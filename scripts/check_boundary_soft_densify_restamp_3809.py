#!/usr/bin/env python3
"""Issue #3809: Boundary Soft live_compact Densify restamp (dual-track vs #3677/#3742).

Outermost ~MutationBoundaryGuard Soft probe ran AFTER BoundarySuccess triad
restamp and AFTER Phase-5 Moving densify without calling
unified_restamp_after_boundary(Densify) when Soft bumped gen / wiped pins.
GC compact_sweep Soft path already restamped in #3677/#3742.

#3809 mirrors that belt at probe_arena_auto_policy_on_boundary_exit:
snapshot gen_at_entry; if Soft advanced gen / invalidated / remapped pins,
Densify restamp before Guard returns. Soft stays non-Moving (no Moving
window publish). Soft soft-gated no-op → zero extra restamp.

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
            fails.append(f"{label}: unexpected {n!r}")

    ixx = _read("src/compiler/evaluator.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    gc = _read("src/compiler/evaluator_gc.cpp")
    test = _read("tests/serve/test_gc_compact_sweep_batch.cpp")
    build = _read("build.py")
    gf = _read("scripts/coverage/simple_check_grandfather.txt")

    probe = ixx.find("void probe_arena_auto_policy_on_boundary_exit")
    live = ixx.find("[[nodiscard]] aura::ast::LiveCompactResult", probe if probe != -1 else 0)
    win = ixx[probe:live] if probe != -1 and live != -1 and live > probe else ""

    # AC1: Soft gen bump restamps Densify before Guard returns
    must("Issue #3809", "AC1 cite probe", win)
    must("gen_at_entry", "AC1 gen snapshot", win)
    must("LiveCompactMode::Soft", "AC1 Soft mode", win)
    must("lc.invalidates_pins || lc.remapped_pins > 0", "AC1 invalidates/remap arm", win)
    must("lc.new_gen != 0 && lc.new_gen != gen_at_entry", "AC1 new_gen arm", win)
    must(
        "unified_restamp_after_boundary(UnifiedRestampSite::Densify)",
        "AC1 Densify restamp",
        win,
    )
    must("probe_arena_auto_policy_on_boundary_exit(success)", "AC1 Guard calls probe", mb)

    # Dual-track retained on GC Soft path
    must("Issue #3677", "AC1 GC Soft cite #3677", gc)
    must("Issue #3742", "AC1 GC Soft cite #3742", gc)

    # AC2: soft-gated no-op still zero extra (unchanged gen → no restamp arm)
    must("soft-gated", "AC2 soft-gated comment", win)

    # AC3 soak covered in batch test
    must("run_3809_boundary_soft_densify_restamp", "AC3 test harness", test)
    must("3809 AC3", "AC3 soak marker", test)

    # AC4: Soft does NOT publish Moving densify health window (call site)
    must("no publish_last_moving_densify_window", "AC4 Soft non-Moving comment", win)
    if "publish_last_moving_densify_window(" in win:
        fails.append("AC4: Soft probe must not call publish_last_moving_densify_window(")
    must("3809 AC4", "AC4 test marker", test)

    must("check_boundary_soft_densify_restamp_3809", "AC wire build.py", build)
    must("Issue #3809", "AC build cite", build)
    must("check_boundary_soft_densify_restamp_3809.py", "AC grandfather", gf)

    for rel in (
        "tests/serve/test_issue_3809.cpp",
        "tests/compiler/test_issue_3809.cpp",
        "tests/issues/test_issue_3809.cpp",
    ):
        if _read(rel):
            fails.append(f"forbidden invent: {rel} exists")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("3809-*.md"):
            fails.append(f"forbidden design doc: {f.relative_to(ROOT)}")

    if fails:
        print(f"Issue #3809 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3809 Boundary Soft Densify restamp — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
