#!/usr/bin/env python3
"""Issue #3356: densify success rewrites pin/EnvFrame/JIT in the dirty cone.

Pins / linear_roots / known-root slots already remap via last_object_remap_
on objects_moved>0. Residual: CompilerService::on_arena_compact_notify
wholesale mark_all_blocks_dirty of ir_cache_v2_ (O(module) deopt) even
when the dirty cone is empty.

Fix: cone-limited finish_dirty_sync of already-dirty entries only.
objects_moved==0 (empty remap) or empty cone → zero extra IR restamp.
Production pin rewrite-miss stays fail-closed (verify_pins_under_moving_compact).
Soft observes only. No new process-wide lock / query key.

Contract:
  AC1 densify success + dirty cone → cone-limited rewrite (no wholesale)
  AC2 empty cone / objects_moved==0 → skip extra IR restamp
  AC3 production pin rewrite-miss fail-closed; Soft observe
  AC4 extend test_moving_densify_fail_closed; #2266/#3350 retained
  AC5 linter AFTER #3350; no docs/design/; no test_issue_3356.cpp;
      no schema-3356 / g_3356_*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _notify_body(svc: str) -> str:
    i = svc.find("void on_arena_compact_notify()")
    if i < 0:
        return ""
    brace = svc.find("{", i)
    if brace < 0:
        return ""
    depth = 1
    j = brace + 1
    while j < len(svc) and depth > 0:
        if svc[j] == "{":
            depth += 1
        elif svc[j] == "}":
            depth -= 1
        j += 1
    return svc[brace:j]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/core/post_compact_lifecycle.hh")
    svc = _read("src/compiler/service.ixx")
    arena = _read("src/core/arena.ixx")
    pin = _read("src/core/lifetime_pin.hh")
    t = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")
    body = _notify_body(svc)

    # AC1 — cone-limited rewrite on densify success.
    must("kDensifyConeRewriteIssue = 3356", "AC1 stamp", hh)
    must("Issue #3356", "AC1 lifecycle cite", hh)
    must("Issue #3356", "AC1 compact hook cite", svc)
    must("object_remap_size()", "AC1 densify success gate", svc)
    must("any_block_dirty()", "AC1 dirty mask", svc)
    must("note_densify_cone_rewrite", "AC1 cone counter", svc)
    must("force_soa_instruction_dirty_sync", "AC1 cone rewrite", svc)
    must("remap_pins_pointing_to", "AC1 pin rewrite", pin + arena)
    if ".mark_all_blocks_dirty(" in body:
        fails.append("AC1: on_arena_compact_notify still wholesale mark_all_blocks_dirty")
    if not body:
        fails.append("AC1: could not extract on_arena_compact_notify body")
    must("ac3356_1_cone_limited_rewrite", "AC1 test", t)

    # AC2 — empty cone / no-move skip.
    must("densify_cone_rewrite_skip_empty_total", "AC2 skip counter", hh)
    must("object_remap_size() > 0", "AC2 moved gate", svc)
    must("ac3356_2_empty_cone_skip", "AC2 test", t)

    # AC3 — production fail-closed pin miss retained.
    must("verify_pins_under_moving_compact", "AC3 pin fail-closed", arena)
    must("ac3356_3_pin_fail_closed", "AC3 test", t)

    # AC4 — extend existing densify suite; #3350 retained.
    must("remap_linear_roots_under_moving", "AC4 #3350 retained", arena)
    must("ac3356_4_existing_suite", "AC4 test", t)

    # AC5 — linter after #3350; no invent.
    must("check_densify_cone_rewrite_3356", "AC5 build.py", build)
    must("ac3356_5_source_and_linter", "AC5 test", t)
    prev = build.find("check_linear_root_moving_remap_3350")
    ours = build.find("check_densify_cone_rewrite_3356")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3350")
    if "schema-3356" in hh or "schema-3356" in svc:
        fails.append("AC5: new schema-3356 query key")
    if "g_3356_" in hh or "g_3356_" in svc or "g_3356_" in arena:
        fails.append("AC5: new g_3356_* counter")
    if _read("tests/core/test_issue_3356.cpp"):
        fails.append("AC5: test_issue_3356.cpp present (forbidden #81967)")
    if _read("tests/issues/test_issue_3356.cpp"):
        fails.append("AC5: tests/issues/test_issue_3356.cpp present (forbidden #81967)")
    if _read("docs/design/3356-densify-cone-rewrite.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3356 densify_cone_rewrite:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3356 densify_cone_rewrite: cone-limited IR rewrite on densify success")
    return 0


if __name__ == "__main__":
    sys.exit(main())
