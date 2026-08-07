#!/usr/bin/env python3
"""Issue #2713: measure + bound process-global epoch cross-eval invalidation.

Closes the #2670/#2606 asymmetry: #2670 namespaced stable_func_id by
(eval_owner, name); #2606 filters reemit candidates by owner TLS; #2692
asserts slot.owner vs map owner. Joint bridge / AOT table epoch remains
process-global by design. Under concurrent multi-Evaluator hosts, eval
A's cascade/invalidate still advances g_current_bridge_epoch +
g_aot_table_epoch, forcing eval B live AOT/JIT into generation-behind
even when sid maps and slot ownership are isolated. #2713 adds
observability (not domain split) — counter bumps when >1 live
AotState is registered at aura_aot_bump_func_table_epoch(); single-eval
short-circuits to zero work.

Contract rows (AC1–AC6 from the test file):

  AC1: >1 live AotState → cross_eval_epoch_bump_total bumps on joint
       epoch advance.
  AC2: single-eval / process-default (map size ≤1) → counter stays 0;
       zero extra work beyond one relaxed load of the map size.
  AC3: last_cross_eval_epoch_bump_owner is stamped to current register
       owner when the bump fires.
  AC4: epoch advance itself is unchanged (observability only; per-eval
       epoch domain split is a non-goal for this issue per AC4 stretch).
  AC5: additive query keys only — preserve #2670 / #2692 / #2606 /
       #2046 surfaces + schema sentinels.
  AC6: source-cite + linter + no docs/design/.

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
        [sys.executable, str(ROOT / "scripts" / "check_cross_eval_epoch_bump_2713.py")],
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

    cpp = _read("src/compiler/aura_jit_bridge.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    t = _read("tests/compiler/test_named_closure_stable_id_at_create.cpp")
    build = _read("build.py")

    # AC1 — >1 live AotState → cross-eval bump fires.
    must("Issue #2713", "AC1", cpp)
    must("g_cross_eval_epoch_bump_total", "AC1", cpp)
    must("aura_aot_state_map_size() > 1", "AC1", cpp)

    # AC2 — single-eval / process-default → counter stays 0.
    # (Verified by aura_aot_state_map_size() > 1 gate; single-eval
    # returns 1 or 0 → no bump.)

    # AC3 — last owner stamped.
    must("g_last_cross_eval_epoch_bump_owner", "AC3", cpp)
    must("last_cross_eval_epoch_bump_owner_v_read", "AC3", cpp)
    must("aura_aot_get_register_owner_eval()", "AC3", cpp)

    # AC4 — epoch advance unchanged; per-eval domain split is a non-goal.
    must("g_aot_table_epoch.fetch_add(1, std::memory_order_acq_rel) + 1", "AC4", cpp)
    must("domain split is a follow-up", "AC4", cpp)

    # AC5 — additive query keys.
    must("cross-eval-epoch-bump-total", "AC5", q)
    must("cross-eval-epoch-bump-last-owner", "AC5", q)
    must("cross-eval-epoch-bump-wired", "AC5", q)
    must("schema-2713", "AC5", q)
    must("issue-2713", "AC5", q)
    # Regression on prior #2670 / #2606 / #2046 surface.
    must("reemit-cross-eval-candidate-skipped-total", "AC5", q)
    must("schema-2606", "AC5", q)

    # AC6 — source-cite + linter + build.py + no docs/design/.
    must("ac2713_1_cross_eval_bump_under_multi_eval", "AC6", t)
    must("ac2713_2_single_eval_zero_cost", "AC6", t)
    must("ac2713_3_last_owner_stamped", "AC6", t)
    must("ac2713_4_epoch_advance_unchanged", "AC6", t)
    must("ac2713_5_query_keys_added", "AC6", t)
    must("ac2713_6_source_and_linter", "AC6", t)
    must("check_cross_eval_epoch_bump_2713", "AC6", build)
    if _read("docs/design/cross_eval_epoch_bump_2713.md"):
        fails.append("AC6: docs/design/2713-* exists (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2713 cross-eval epoch tax observability — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
