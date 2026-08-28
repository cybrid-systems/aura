#!/usr/bin/env python3
"""Issue #3324: abort dual-topology restore must not clean-hit pre-abort IR.

Residual after #3033/#3069/#3117/#3184/#3258: force_ir_cache_dirty_after_abort
clears source_to_ir_map + sets abort_map_invalid + zeros stamps, but
lookup_define_v2 did not consult abort_map_invalid, and relower_define_blocks
could instr-peel / restamp live / clear dirty on pre-abort IR. A later
lookup then served a clean hit.

Contract:
  AC1  lookup_define_v2 returns need-relower while abort_map_invalid
  AC2  recover abort-stale skips partial patch (no pre-abort NodeIds)
  AC3  relower_define_blocks disables partial peel when abort-stale
  AC4  force_dirty still clears map + abort_map_invalid; Soft never-aborted
       gen==0 unchanged
  AC5  extend test_abort_ir_cache_fence_first; linter; no invent / docs /
       schema-3324 / g_3324_*

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
    pure = _read("src/compiler/ir_cache_pure.ixx")
    t = _read("tests/compiler/test_abort_ir_cache_fence_first.cpp")
    build = _read("build.py")

    lu = svc.find("int lookup_define_v2(")
    lu_win = svc[lu : lu + 2500] if lu >= 0 else ""
    must("Issue #3324", "AC1 lookup cite", lu_win)
    must("abort_map_invalid", "AC1 lookup flag", lu_win)
    must("ac3324_1_lookup_refuses_after_abort", "AC1 test", t)

    must("force_full_rebuild", "AC2 recover param", pure)
    must("do not partial-patch a pre-abort map", "AC2 skip patch", pure)
    must("ac3324_2_map_not_green_with_preabort_ids", "AC2 test", t)

    rb = svc.find("bool relower_define_blocks(")
    rb_win = svc[rb : rb + 1800] if rb >= 0 else ""
    must("abort_stale_map", "AC3 relower gate", rb_win)
    must("Issue #3324", "AC3 relower cite", rb_win)
    must("ac3324_3_relower_skips_partial", "AC3 test", t)

    fd = svc.find("void force_ir_cache_dirty_after_abort()")
    fd_win = svc[fd : fd + 3200] if fd >= 0 else ""
    must("source_to_ir_map.clear()", "AC4 clear map", fd_win)
    must("abort_map_invalid = true", "AC4 invalid flag", fd_win)
    must("Issue #3324", "AC4 force-dirty cite", fd_win)
    must("abort_map_invalid", "AC4 lookup face", fd_win)
    must("if (gen == 0)", "AC4 gen==0 skip", svc)
    must("ac3324_4_recover_force_full", "AC4 test", t)

    must("check_abort_restore_stale_map_stamp_3324", "AC5 build.py", build)
    must("ac3324_5_source_and_linter", "AC5 test", t)
    prev = build.find("check_abort_restore_force_dirty_3184")
    ours = build.find("check_abort_restore_stale_map_stamp_3324")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3184")
    if "schema-3324" in svc or "g_3324_" in svc:
        fails.append("AC5: new schema-3324 / g_3324_*")
    if _read("tests/compiler/test_issue_3324.cpp") or _read("tests/issues/test_issue_3324.cpp"):
        fails.append("AC5: test_issue_3324.cpp present (forbidden #81967)")
    if _read("docs/design/3324-abort-restore-stale-map.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3324 abort_restore_stale_map_stamp:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3324 abort_restore_stale_map_stamp: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
