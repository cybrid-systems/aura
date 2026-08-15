#!/usr/bin/env python3
"""Issue #3070: Shape→None hysteresis + forced-thr freeze + peer soft-stale.

Storm exit keeps Global-force-full for a cooldown when deopt window is
still elevated. Forced-wide thr is ignored under an active storm bit.
Owner-scoped invalidate marks peer AOT slots soft-stale without bumping
g_aot_table_epoch.

Contract:
  AC1 Shape+deopt exit: cooldown force-full; reset clears
  AC2 Forced thr cannot stay wide under Global
  AC3 Peer probe misses pre-invalidate native; epoch unchanged
  AC4 extend storm + owner-scoped suites; linter; no docs/design/;
      no test_issue_3070; no new query keys

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    pure = _read("src/compiler/ir_cache_pure.ixx")
    reg = _read("src/compiler/hot_update_registry.cpp")
    regh = _read("src/compiler/hot_update_registry.hh")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    q = read_query_prims() + _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    storm = _read("tests/compiler/test_shape_storm_partial_relower.cpp")
    owner = _read("tests/compiler/test_named_closure_stable_id_at_create.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3070", "AC1 pure", pure)
    must("storm_exit_force_full_active", "AC1 gate", pure)
    must("Issue #3070", "AC1 registry", reg)
    must("storm_exit_force_full_remaining_", "AC1 cooldown", regh)
    must("ac3070_hysteresis_and_forced_thr", "AC1 test", storm)
    must("cooldown force-full after Shape→None", "AC1 soak", storm)

    # AC2
    must("partial_relower_threshold_is_forced()", "AC2 forced check", pure)
    must("kDefaultPartialRelowerThreshold", "AC2 default base", pure)
    must("forced-wide not kept under Global", "AC2 test", storm)

    # AC3
    must("soft_stale", "AC3 slot bit", br)
    must("aura_aot_mark_peer_slots_soft_stale", "AC3 mark", br)
    must("Issue #3070", "AC3 bridge", br)
    must("ac3070_1_peer_soft_stale_no_epoch_force", "AC3 test", owner)
    must("no global epoch force", "AC3 epoch", owner)

    # AC4
    must("check_storm_exit_hysteresis_peer_soft_stale_3070", "AC4 build", build)
    must("cmd_storm_exit_hysteresis_peer_soft_stale_3070", "AC4 cmd", build)
    if "schema-3070" in q:
        fails.append("AC4: new query key schema-3070 (forbidden)")
    if (ROOT / "tests" / "compiler" / "test_issue_3070.cpp").is_file():
        fails.append("AC4: test_issue_3070.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*3070*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3070 storm-exit hysteresis + peer soft-stale — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
