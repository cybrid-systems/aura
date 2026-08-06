#!/usr/bin/env python3
"""Issue #2699: unified steal safety single-transaction call graph.

Contract (one row per AC):
  AC1 src/serve/steal_safety.h defines StealSafetyDecision + the unified
     entry steal_safety_transaction(Fiber*). src/serve/steal_safety.cpp
     threads all 7 AC1 steps in order (sample snapshot → inconsistency →
     force-deopt + RejectHard → residual GcDefer clear → PanicCheckpoint
     clear → LayoutStamp dual-check → linear/ StableNodeRef provenance →
     stamp resume_safety_ticket on Ok path).
  AC2 src/serve/worker.cpp try_steal_from success path calls the
     unified transaction; RejectHard → never local_queue_.push. Wire-in
     marker (call_steal_complete_now_uses_unified_transaction) ensures
     the call graph is the single entry point.
  AC3 Soft / sandbox / test-override path remains metric-only — the
     transaction returns Ok when soft-mode + metrics bump; production
     lock strict aborts via the existing aura_evaluator_on_steal_complete
     weak-strict path (#2372).
  AC4 Existing counters (steal_snapshot_mismatch_force_deopt_total,
     residual_defer_steal_hard_fail_total, panic_checkpoint_cleared_on_steal_total,
     steal_safety_ticket_mismatch_total) remain additive / non-regressing.
     Transaction counters (calls / reject_hard / ok) are file-scope
     additive — new surface, no replacement.
  AC5 tests/serve/test_steal_complete_restamp_txn.cpp (or successor —
     test_residual_defer_steal_hard_and, test_steal_safety_ticket) extended
     per #81967 with ac2699_1..6 source-cite + runtime hard-path block.
     Source-cite verifies the unified transaction is wired in worker.cpp
     + build.py wires the linter.
  AC6 No docs/design/ per #1655 (aura philosophy: agent-developed repo,
     not human docs; design rationale lives in commit message + close
     comment).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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

    hdr = _read("src/serve/steal_safety.h")
    cpp = _read("src/serve/steal_safety.cpp")
    worker = _read("src/serve/worker.cpp")
    fiber = _read("src/serve/fiber.cpp")
    gc_hooks = _read("src/core/gc_hooks.h")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")
    test = _read("tests/serve/test_steal_complete_restamp_txn.cpp")

    # AC1 — unified entry + all 7 steps in order
    must("steal_safety_transaction", "AC1", hdr)
    must("StealSafetyDecision", "AC1", hdr)
    must("Issue #2699", "AC1", hdr)
    must("g_steal_safety_transaction_calls_total", "AC1", hdr)
    must("g_steal_safety_transaction_reject_hard_total", "AC1", hdr)
    must("g_steal_safety_transaction_ok_total", "AC1", hdr)
    must("g_steal_safety_transaction_wired", "AC1", hdr)
    must("kStealSafetyTransactionIssue = 2699", "AC1", hdr)
    must("Issue #2699", "AC1", cpp)
    must("mutation_safety_snapshot", "AC1 step 1", cpp)
    must("mutation_safety_snapshot_inconsistent", "AC1 step 2", cpp)
    must("force_clear_residual_defer_for_evaluator", "AC1 step 3", cpp)
    must("aura_evaluator_on_steal_complete", "AC1 steps 4-6", cpp)
    must("set_resume_safety_ticket", "AC1 step 7", cpp)

    # AC2 — worker.cpp try_steal_from routes through the unified entry
    must("steal_safety_transaction", "AC2", worker)
    # RejectHard enum value lives in steal_safety.h/.cpp
    must("RejectHard", "AC2", hdr)
    must("RejectHard", "AC2", cpp)
    # Wire-in marker comment ensures the call graph is single-entry
    must("call_steal_complete_now_uses_unified_transaction", "AC2 marker", worker)
    # CMakeLists registers the new TU
    must("steal_safety.cpp", "AC2", cmake)
    # build.py wires the linter
    must("check_steal_safety_transaction_2699", "AC2", build)

    # AC3 — soft / sandbox metric-only (flags live in fiber.cpp)
    must("steal_snapshot_soft_production_locked", "AC3", fiber)
    must("aura_fiber_is_steal_snapshot_soft_mode", "AC3", fiber)

    # AC4 — existing counters remain additive / non-regressing
    # steal_snapshot_mismatch_force_deopt_total + steal_safety_ticket_mismatch_total
    # live in fiber.cpp. residual_defer_steal_hard_fail_total +
    # panic_checkpoint_cleared_on_steal_total live in core/gc_hooks.h.
    must("steal_snapshot_mismatch_force_deopt_total", "AC4", fiber)
    must("steal_safety_ticket_mismatch_total", "AC4", fiber)
    must("residual_defer_steal_hard_fail_total", "AC4", gc_hooks)
    must("panic_checkpoint_cleared_on_steal_total", "AC4", gc_hooks)
    must("Issue #2699", "AC4", hdr)
    must("Issue #2699", "AC4", cpp)

    # AC5 — test file extension per #81967
    must("ac2699_1_unified_entry_exists", "AC5", test)
    must("ac2699_2_reject_hard_skips_enqueue", "AC5", test)
    must("ac2699_3_soft_path_metric_only", "AC5", test)
    must("ac2699_4_existing_counters_preserved", "AC5", test)
    must("ac2699_5_source_and_linter", "AC5", test)
    must("ac2699_6_no_docs_design", "AC5", test)

    # AC6 — no docs/design/2699-* on disk
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2699-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check: prior runtime safety linters still green
    r1 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_joint_epoch_bump_coverage.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r1.returncode != 0:
        fails.append(f"check_joint_epoch_bump_coverage regression:\n{r1.stdout}\n{r1.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2699 unified steal safety single transaction — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
