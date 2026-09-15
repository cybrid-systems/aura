#!/usr/bin/env python3
"""Issue #3822: Production dirty suite SoA PureWrap must sync into AoS writeback.

Under Production + non-empty soa_mod, suite skipped AoS PureWrap and ran SoA
Escape + hot pack on soa_mod, but writeback SSOT is AoS entry.irs only — SoA
body mutations were not mirrored. Soft + soa_hot ran both tracks.

Contract (one row per AC):
  AC1  prod_soa: hot pack then sync_soa_dirty_blocks_into_aos into ir_mod
  AC2  Soft: hot pack gated on prod_soa (not bare soa_hot); AoS suite kept
  AC3  Soak: suite cites invocations only with writeback sync companion
  AC4  Tests extend test_soa_dirty_aware_pipeline; no invent / docs/design

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

    svc = _read("src/compiler/service.ixx")
    soa = _read("src/compiler/ir_soa.ixx")
    t = _read("tests/compiler/test_soa_dirty_aware_pipeline.cpp")
    build = _read("build.py")

    must("Issue #3822", "AC1 cite", svc)
    must("sync_soa_dirty_blocks_into_aos", "AC1 sync helper", soa)
    must("sync_soa_dirty_blocks_into_aos", "AC1 suite sync", svc)

    prod = svc.find("const bool prod_soa")
    if prod < 0:
        fails.append("AC1: prod_soa missing")
        win = ""
    else:
        win = svc[prod : prod + 4000]
        must("if (!prod_soa)", "AC2 Soft AoS suite", win)
        arms = []
        start = 0
        while True:
            i = win.find("if (prod_soa)", start)
            if i < 0:
                break
            arms.append(win[i : i + 500])
            start = i + 1
        if not arms:
            fails.append("AC1: no if (prod_soa) hot-pack arm")
        else:
            arm = arms[-1]
            must("run_production_soa_dirty_hot_pack", "AC1 hot pack", arm)
            must("sync_soa_dirty_blocks_into_aos", "AC1 sync after pack", arm)
        if "if (soa_hot)" in win:
            fails.append("AC2: suite still gates hot pack on bare soa_hot")
        must("beyond Soft contract", "AC2 Soft contract", win)

    must("writeback SSOT", "AC3 writeback cite", svc)
    must("production_soa_dirty_hot_pack_invocations_total", "AC3 soak cite", svc)

    must("3822 AC1: AoS dirty const matches SoA", "AC4 AC1 test", t)
    must("3822 Soft: suite no longer gates hot pack on bare soa_hot", "AC4 Soft test", t)
    must("3822 soak: hot_pack invocations advance with sync companion", "AC4 soak", t)
    must("check_prod_soa_writeback_sync_3822", "AC4 build.py", build)

    if (ROOT / "tests" / "compiler" / "test_issue_3822.cpp").is_file():
        fails.append("AC4: test_issue_3822.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3822.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3822.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3822-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print(f"Issue #3822 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3822 prod_soa writeback sync — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
