#!/usr/bin/env python3
"""Issue #2250: LayoutStamp fence on Fiber resume/steal.

Contract (5 AC from issue body):
  AC1: Phase 5 of outermost Guard writes current LayoutStamp into
       current Fiber (before unlock).
  AC2: Fiber::resume / refresh_stale_frames_after_steal hard-compares
       fiber-stored stamp vs Evaluator::current_layout_stamp(); any
       field mismatch -> bump layout_stamp_resume_mismatch_total +
       force scan_live_closures_for_linear_captures(true, false).
  AC3: Zero-cost when stamps match (relaxed loads only).
  AC4: Metric + query surface (additive keys on query:stable-ref-stats
       + schema-2250 lineage).
  AC5: Unit/integration dual-worker steal + concurrent reemit AC.

This linter is the source-of-truth for the production surface. A
ship is incomplete if any contract row fails.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = REPO / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8")


def _must(cond: bool, msg: str, fails: list) -> None:
    if not cond:
        fails.append(msg)


def check() -> list:
    fails = []

    fiber_h = _read("src/serve/fiber.h")
    mut_boundary = _read("src/compiler/evaluator_mutation_boundary.cpp")
    fiber_mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    met = _read("src/compiler/observability_metrics.h")
    eval_ixx = _read("src/compiler/evaluator.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_layout_stamp_2170.cpp")

    # AC1 — Fiber POD has 6 stamp fields + has_resume_layout_stamp set flag
    _must("set_resume_layout_stamp" in fiber_h, "AC1: Fiber::set_resume_layout_stamp helper missing", fails)
    _must(
        "resume_arena_gen" in fiber_h
        and "resume_flat_gen" in fiber_h
        and "resume_mutation_epoch" in fiber_h
        and "resume_env_gen" in fiber_h
        and "resume_defuse" in fiber_h,
        "AC1: 5/6 resume LayoutStamp fields missing in fiber.h",
        fails,
    )
    _must("has_resume_layout_stamp" in fiber_h, "AC1: has_resume_layout_stamp check missing", fails)
    # AC1 — Phase 5 wire-up
    _must(
        mut_boundary.find("set_resume_layout_stamp(") != -1,
        "AC1: Phase 5 wire-up (set_resume_layout_stamp call) missing",
        fails,
    )
    _must(
        mut_boundary.find("current_layout_stamp()") != -1, "AC1: Phase 5 uses current_layout_stamp() to capture", fails
    )
    # AC1 ordering: publish_layout_stamp must come before set_resume_layout_stamp
    # (with possible metric bumps + Fiber-set code in between). Both
    # calls must be inside the outermost Phase 5 path.
    # Issue #2436: stamp is re-published *after* densify so the first
    # publish_layout_stamp() may be early; require the publish immediately
    # preceding set_resume (rfind) within proximity, not the file's first.
    set_idx = mut_boundary.find("set_resume_layout_stamp(")
    pub_idx = mut_boundary.rfind("publish_layout_stamp();", 0, set_idx if set_idx != -1 else 0)
    _must(
        pub_idx != -1 and set_idx != -1 and pub_idx < set_idx and (set_idx - pub_idx) < 1500,
        "AC1: set_resume_layout_stamp must come AFTER publish_layout_stamp (Phase 5 ordering)",
        fails,
    )

    # AC2 — hard compare + bump + force dual-check
    _must(fiber_mut.find("layout_stamp_resume_mismatch_total") != -1, "AC2: mismatch counter bump site missing", fails)
    _must(
        fiber_mut.find("scan_live_closures_for_linear_captures(true, false)") != -1,
        "AC2: force dual-check call missing",
        fails,
    )
    _must(fiber_mut.find("resume_arena_id() != cur.arena_id") != -1, "AC2: hard compare fence missing", fails)
    _must(fiber_mut.find("clear_resume_layout_stamp") != -1, "AC2: clear_resume_layout_stamp (one-shot) missing", fails)

    # AC3 — zero-cost when stamps match
    _must(
        fiber_mut.find("if (fiber->has_resume_layout_stamp())") != -1, "AC3: zero-cost skip when no resume stamp", fails
    )

    # AC4 — metric + query + schema-2250
    _must(
        "layout_stamp_resume_mismatch_total{0}" in met, "AC4: counter field missing in observability_metrics.h", fails
    )
    _must("layout-stamp-resume-mismatch-total" in q, "AC4: query key missing", fails)
    _must("layout-stamp-resume-wired" in q, "AC4: layout-stamp-resume-wired sentinel missing", fails)
    _must("schema-2250" in q and "issue-2250" in q, "AC4: schema-2250 / issue-2250 lineage missing", fails)
    _must(
        "get_layout_stamp_resume_mismatch_total" in eval_ixx,
        "AC4: accessor declaration missing in evaluator.ixx",
        fails,
    )
    _must(
        "Evaluator::get_layout_stamp_resume_mismatch_total()" in mut_boundary,
        "AC4: accessor implementation missing",
        fails,
    )

    # AC5 — dual-worker integration AC (test file)
    _must("ac2250_fiber_resume_fence" in test, "AC5: ac2250_fiber_resume_fence test function missing", fails)
    _must("#2250" in test, "AC5: #2250 issue citation missing in test file comment", fails)

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2250 LayoutStamp fence coverage linter")
    parser.add_argument("--self-test", action="store_true", help="Run self-test (return 0 if contract satisfied)")
    parser.add_argument("--strict", action="store_true", help="Strict mode (non-zero exit on any failure)")
    args = parser.parse_args()

    fails = check()
    if args.self_test:
        print(f"self-test: {len(fails)} failures")
        return 0 if not fails else 1

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1

    print("OK: LayoutStamp fence coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
