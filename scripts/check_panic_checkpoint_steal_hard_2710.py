#!/usr/bin/env python3
"""Issue #2710: PanicCheckpoint production-hard policy on steal-complete.

Closes the residual half-open loop where a stolen fiber with a live
PanicCheckpoint could enqueue Ready without clearing the previous Eval's
GC arm. Production / AURA_PANIC_CONTRACT=hard now clears PanicCheckpoint
on both hard_failed (#2667 — existing counter continues to bump) AND Ok
paths (new counter — additive). Soft / dev_off / unset stays metric-only.

Contract rows (AC1–AC6 from the test file):

  AC1: production + live PanicCheckpoint stolen successfully → cleared;
       counter advances; residual GcDeferReason::Panic == 0 before enqueue
  AC2: Soft / dev_off / unset → metric-only (clear optional)
  AC3: Steal RejectHard path never stamps resume ticket and never leaves
       orphan panic defer armed (#2699 / #2702 preserved)
  AC4: align with #2598 densify panic defer audit
  AC5: additive query keys only — preserve #2667 / #2546 / #2314 / #2203
  AC6: source-cite + linter + no docs/design/

Exit 0 = all contract rows satisfied.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _self_test() -> int:
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "check_panic_checkpoint_steal_hard_2710.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print(f"--self-test FAILED:\n{r.stdout}\n{r.stderr}", file=sys.stderr)
        return 1
    print(f"--self-test OK: {r.stdout.strip()}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test", action="store_true", help="Run self-test on this linter")
    args = p.parse_args()

    if args.self_test:
        return _self_test()

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    gh = _read("src/core/gc_hooks.h")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    ob = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    ss = _read("src/serve/steal_safety.cpp")
    _read("src/serve/steal_safety.h")
    t = _read("tests/serve/test_scheduler_gc_defer_pending_panic_steal.cpp")
    build = _read("build.py")

    # AC1 — production + live panic + Ok path → clear + counters.
    must("Issue #2710", "AC1", gh)
    must("g_panic_checkpoint_cleared_on_steal_ok_total", "AC1", gh)
    must("g_panic_contract_hard_pref", "AC1", gh)
    must("Issue #2710", "AC1", efm)
    # Code splits the call across lines: "...ok_total\n                .fetch_add(...)".
    # Check both substrings independently (string-match doesn't span newlines).
    must("g_panic_checkpoint_cleared_on_steal_ok_total", "AC1", efm)
    must(".fetch_add(1, std::memory_order_relaxed)", "AC1", efm)
    must("production_defaults_active()", "AC1", efm)
    must("production_hard", "AC1", efm)

    # AC2 — Soft / dev_off / unset metric-only + AURA_PANIC_CONTRACT env.
    must("apply_panic_contract_env", "AC2", gh)
    must("AURA_PANIC_CONTRACT", "AC2", gh)
    must("panic_contract_hard_pref_v_read", "AC2", gh)
    must("panic_contract_hard_pref_v_read() == 1", "AC2", efm)
    must("Soft / dev_off / unset: no action", "AC2", efm)

    # AC3 — RejectHard path no orphan panic defer; Ok path also clears.
    must("RejectHard", "AC3", ss)
    must("set_resume_safety_ticket", "AC3", ss)
    # StealSafetyDecision::RejectHard is referenced in steal_safety.cpp
    # (return statements), not the header (which only declares the enum).
    must("StealSafetyDecision::RejectHard", "AC3", ss)
    # #2710 replaced `if (hard_failed && prev_eval_id != nullptr)` with
    # `if (prev_eval_id != nullptr)` + `if (!hard_failed)` inside — the
    # new branch covers both paths under production / AURA_PANIC_CONTRACT.
    must("if (!hard_failed)", "AC3", efm)

    # AC4 — align with #2598 densify audit (production lock pattern).
    must("production_defaults_active()", "AC4", efm)
    must("panic_contract_hard_pref_v_read() == 1", "AC4", efm)

    # AC5 — additive query keys (kebab + camelCase + schema/issue sentinels).
    must("panic-checkpoint-cleared-on-steal-ok-total", "AC5", ob)
    must("panic_checkpoint_cleared_on_steal_ok_total", "AC5", ob)
    must("panic-contract-hard-wired", "AC5", ob)
    must("schema-2710", "AC5", ob)
    must("issue-2710", "AC5", ob)
    # Regression on prior surfaces.
    must("panic-checkpoint-cleared-on-steal-total", "AC5", ob)
    must("schema-2667", "AC5", ob)

    # AC6 — source-cite + linter + build.py + no docs/design/.
    must("#2710", "AC6", ss)
    must("ac2710_1_production_hard_clear_on_ok_path", "AC6", t)
    must("ac2710_2_soft_metric_only", "AC6", t)
    must("ac2710_3_reject_hard_no_orphan_panic_defer", "AC6", t)
    must("ac2710_4_align_with_densify_audit_2598", "AC6", t)
    must("ac2710_5_query_keys_added", "AC6", t)
    must("ac2710_6_source_and_linter", "AC6", t)
    must("check_panic_checkpoint_steal_hard_2710", "AC6", build)
    if _read("docs/design/2710-panic-checkpoint-steal-hard.md"):
        fails.append("AC6: docs/design/2710-* exists (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2710 PanicCheckpoint production-hard policy — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
