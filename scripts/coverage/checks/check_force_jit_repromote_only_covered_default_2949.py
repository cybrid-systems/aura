#!/usr/bin/env python3
"""Issue #2949: production default force_jit_repromote only_covered bits.

Refine #2895/#2502 — production_defaults → only_covered partial re-promote;
Soft / sandbox=off wholesale; env=0 opt-out; sticky set wins.

Contract (one row per AC):
  AC1  production resolve only_covered; partial clear residual
  AC2  Soft / sandbox=off / env=0 / set(false) → wholesale
  AC3  quiet path mask==0 short-circuit retained
  AC4  #2502 window/storm/on_reload_success + #2895 stamp preserved
  AC5  schema-2949 additive; schema-2895/2502 preserved
  AC6  tests + build.py; no invent/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/compiler/hot_update_registry.hh")
    cpp = _read("src/compiler/hot_update_registry.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_force_jit_repromote.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2949", "AC1", cpp)
    must("resolve_force_jit_repromote_only_covered", "AC1", cpp)
    must("resolve_force_jit_repromote_only_covered", "AC1", hh)
    must("force_jit_repromote_only_covered_override_", "AC1", hh)
    must("2949 AC1", "AC1", test)

    # AC2
    must("AURA_FORCE_JIT_REPROMOTE_ONLY_COVERED", "AC2", cpp)
    must("AURA_SANDBOX", "AC2", cpp)
    must("aura_production_defaults_active_probe", "AC2", cpp)
    must("2949 AC2", "AC2", test)

    # AC3
    must("mask == 0", "AC3", cpp)
    must("2949 AC3", "AC3", test)

    # AC4
    must("maybe_force_jit_repromote_on_clean_success", "AC4", cpp)
    must("on_reload_success", "AC4", cpp)
    must("last_reemit_success_region_mask_", "AC4", cpp)
    must("2949 AC4", "AC4", test)

    # AC5
    must("schema-2949", "AC5", mut)
    must("issue-2949", "AC5", mut)
    must("force-jit-repromote-only-covered-default-wired", "AC5", mut)
    must("schema-2895", "AC5", mut)
    must("schema-2502", "AC5", mut)
    must("schema_2949", "AC5", hh)

    # AC6
    must("ac2949", "AC6", test)
    must("check_force_jit_repromote_only_covered_default_2949", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2949.cpp").is_file():
        fails.append("AC6: test_issue_2949.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2949-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Wire path uses resolve (not raw sticky-only)
    if "resolve_force_jit_repromote_only_covered()" not in cpp:
        fails.append("AC6: maybe_force must call resolve_force_jit_repromote_only_covered()")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2949 production force_jit_repromote only_covered default")
    return 0


if __name__ == "__main__":
    sys.exit(main())
