#!/usr/bin/env python3
"""Issue #2436: post-compact Arena × IR SoA × Shape × fiber lifecycle.

Contract:
  AC1 documented ordered lifecycle (post_compact_lifecycle.hh)
  AC2 Phase 5 + service compact hook ordered (stamp after compact)
  AC3 pin-or-remap hard path + lifecycle pin-fail counter
  AC4 LayoutStamp publish after densify + shape/IR close
  AC5 soft_skip path + schema-2436

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    hh = _read("src/core/post_compact_lifecycle.hh")
    mut = _read("src/compiler/evaluator_mutation_boundary.cpp")
    svc = _read("src/compiler/service.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_post_compact_lifecycle_2436.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2436", "AC1", hh)
    must("finish_dirty_sync", "AC1", hh)
    must("on_arena_compact", "AC1", hh)
    must("LayoutStamp", "AC1", hh)
    must("kPostCompactLifecycleIssue", "AC1", hh)
    must("2436 AC1", "AC1", test)

    # AC2
    must("note_lifecycle_run", "AC2", mut)
    must("note_lifecycle_soft_skip", "AC2", mut)
    must("force_soa_instruction_dirty_sync", "AC2", svc)
    must("note_lifecycle_ir_sync", "AC2", svc)
    must("2436 AC2", "AC2", test)

    # AC3
    must("note_lifecycle_pin_fail", "AC3", mut)
    must("AURA_MOVING_PIN_CONTRACT", "AC3", mut)
    must("2436 AC3", "AC3", test)

    # AC4 — stamp publish after compact (source-cite Issue #2436 on stamp)
    must("note_lifecycle_stamp_publish", "AC4", mut)
    must("Issue #2436 AC4", "AC4", mut)
    must("2436 AC4", "AC4", test)

    # AC5
    must("post_compact_lifecycle_soft_skip_total", "AC5", hh)
    must("schema-2436", "AC5", q)
    must("post-compact-lifecycle-wired", "AC5", q)
    must("2436 AC5", "AC5", test)
    must("check_post_compact_lifecycle_2436", "gate", build)
    must("cmd_post_compact_lifecycle_coverage", "gate", build)
    must("test_post_compact_lifecycle_2436", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: post compact lifecycle #2436 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
