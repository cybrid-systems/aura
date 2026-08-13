#!/usr/bin/env python3
"""Issue #2950: pure-anon pressure-driven background remount queue.

Close #2893 residual: budget-exhausted pure-anon enqueued for bg remount
on BoundaryExit / pipeline path (never steal-complete #2715).

Contract (one row per AC):
  AC1  budget skip enqueues; drain remounts
  AC2  Soft / budget=0 → no enqueue
  AC3  steal-complete never drains
  AC4  pure-anon filters only; named/captured unchanged
  AC5  schema-2950 additive; #2893/#2850 preserved
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

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    sh = _read("src/compiler/runtime_shared.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    met = _read("src/compiler/observability_metrics.h")
    reg = _read("src/compiler/hot_update_registry.cpp")
    mbc = _read("src/compiler/evaluator_mutation_boundary.cpp")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2950", "AC1", rt)
    must("aura_pure_anon_bg_enqueue", "AC1", rt)
    must("aura_pure_anon_bg_remount_drain", "AC1", rt)
    must("kPureAnonBgQueueCap", "AC1", rt)
    must("pending_bg", "AC1", rt)
    must("2950 AC1", "AC1", test)

    # AC2
    must("budget == 0", "AC2", rt)
    must("pure_budget > 0", "AC2", br)
    must("2950 AC2", "AC2", test)

    # AC3
    must("aura_pure_anon_bg_remount_drain", "AC3", reg)
    must("aura_pure_anon_bg_remount_drain", "AC3", mbc)
    must("never steal", "AC3", rt)
    # steal-complete function must not call drain
    pos = steal.find("aura_evaluator_on_steal_complete")
    if pos < 0:
        fails.append("AC3: steal-complete site missing")
    else:
        win = steal[pos : pos + 8000]
        if "aura_pure_anon_bg_remount_drain" in win:
            fails.append("AC3: steal-complete must not call pure-anon bg drain")
    must("2950 AC3", "AC3", test)

    # AC4
    must("sid != 0", "AC4", rt)
    must("aura_closure_has_env_or_linear_captures", "AC4", rt)
    must("2950 AC4", "AC4", test)

    # AC5
    must("schema-2950", "AC5", obs)
    must("pure-anon-bg-enqueue-total", "AC5", obs)
    must("pure-anon-bg-drain-ok-total", "AC5", obs)
    must("pure-anon-bg-drain-fail-total", "AC5", obs)
    must("pure-anon-bg-overflow-total", "AC5", obs)
    must("pure-anon-bg-remount-wired", "AC5", obs)
    must("pure_anon_bg_enqueue_total", "AC5", met)
    must("schema-2893", "AC5", obs)
    must("schema-2850", "AC5", obs)
    must("aura_bump_pure_anon_bg_totals", "AC5", br)

    # AC6
    must("aura_pure_anon_bg_enqueue", "AC6", sh)
    must("aura_pure_anon_bg_enqueue", "AC6", stub)
    must("ac2950", "AC6", test)
    must("check_pure_anon_bg_remount_2950", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2950.cpp").is_file():
        fails.append("AC6: test_issue_2950.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2950-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2950 pure-anon bg remount queue")
    return 0


if __name__ == "__main__":
    sys.exit(main())
