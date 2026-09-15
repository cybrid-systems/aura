#!/usr/bin/env python3
"""Issue #3820: solve_delta pending clear-after-offer must not drop
var_to_constraints_ map-miss seeds under production/Full.

collect_for_root no-ops on map miss (densify/steal remount — #3253 family);
unconditional pending_full_solve_roots_.clear() after offer silently dropped
those seeds. Soft keeps clear-after-offer; Prod/Full retain misses.

Contract (one row per AC):
  AC1  Prod/Full: miss retain via production_hard_face_active + swap
  AC2  Soft: clear-after-offer; observe unchanged on collect hit
  AC3  Soak: remount miss × pending × next solve_delta retain
  AC4  Tests extend test_solve_delta_unresolved_export; no invent / docs/design

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

    impl = _read("src/compiler/type_checker_impl.cpp")
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")

    start = impl.find("SolveResult ConstraintSystem::solve_delta_impl(")
    if start < 0:
        fails.append("AC1: solve_delta_impl missing")
        win = ""
    else:
        end = impl.find("ConstraintSystem::try_instance_repair_before_full", start)
        if end < 0:
            end = start + 12000
        win = impl[start:end]
        must("Issue #3820", "AC1 cite", win)
        must("production_hard_face_active()", "AC1 hard-face", win)
        must("retain_miss", "AC1 retain set", win)
        must("pending_full_solve_roots_.swap(retain_miss)", "AC1 swap", win)
        must("return false; // miss — do not mark processed (#3820)", "AC1 miss no-mark", win)
        must("Soft / all-hits", "AC2 Soft clear cite", win)
        bad = (
            "for (auto root : pending_full_solve_roots_)\n"
            "            collect_for_root(root);\n"
            "        // Pending roots have been offered a collection pass; clear so\n"
            "        // the next prune can re-queue only what remains non-local.\n"
            "        pending_full_solve_roots_.clear();"
        )
        if bad in win:
            fails.append("AC1: unconditional pending clear-after-offer still present")

    must("ac3820_1_prod_miss_retains_pending", "AC1 test", t)
    must("3820 AC1: pending miss retained under Prod", "AC1 CHECK", t)
    must("ac3820_2_soft_hit_clears_observe_unchanged", "AC2 test", t)
    must("3820 AC2: Soft clear-after-offer", "AC2 clear", t)
    must("3820 AC2: Soft observe unchanged on hit", "AC2 observe", t)
    must("ac3820_3_soak_remount_next_delta", "AC3 soak", t)
    must("3820 soak: next solve_delta does not silently drop pending", "AC3 CHECK", t)
    must("ac3820_4_full_without_prod_retains", "AC4 Full", t)
    must("Full-without-prod", "AC4 cite", t)
    must("check_pending_offer_map_miss_retain_3820", "AC4 build", build)

    if (ROOT / "tests" / "compiler" / "test_issue_3820.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3820.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3820.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3820.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3820-*")):
            fails.append(f"AC4: docs/design/{f.name} present")
    must_not("schema-3820", "AC4 no query key", impl)
    must_not("g_3820_", "AC4 no g_3820_*", impl)

    if fails:
        for e in fails:
            print(f"FAIL: {e}", file=sys.stderr)
        return 1
    print("OK: Issue #3820 pending offer map-miss retain (prod||Full)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
