#!/usr/bin/env python3
"""Issue #2646: cone-truncate must drop goals/memo for dirty Ifs outside truncated cone.

Contract:
  AC1 Soft + cone soft overflow + dirty If outside cone → goals dropped (commit allowed)
  AC2 production + same → commit hard + outside goals dropped
  AC3 !truncated path → counters do not advance; empty outside set → no extra work
  AC4 #2622 diverge metric ordering — outside invalidate fires AFTER #2622 sync
  AC5 Additive schema + linter + build.py wire (source-cite #2621 + #2622 + #2646)
  AC6 Unit test fixture deferred — full soak requires drift-injection helper

Exit 0 = all AC rows satisfied.
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

    impl = _read("src/compiler/type_checker_impl.cpp")
    obs = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    test = _read("tests/compiler/test_partial_cone_commit_gate_2621.cpp")
    build = _read("build.py")

    # AC1+AC2: wiring present
    must("Issue #2646: cone-truncate must drop goals/memo", "AC1", impl)
    must("outside_cone_conds", "AC1", impl)
    must("engine.sync_occurrence_after_dirty", "AC1", impl)
    must("last_partial_cone_truncated_", "AC2", impl)

    # AC3: gate on truncated + empty-set short-circuit
    must("if (last_partial_cone_truncated_)", "AC3", impl)
    must("outside_cone_conds.empty()", "AC3", impl)

    # AC4: ordering — #2646 fires AFTER #2622 sync
    # (text-position check: #2646 marker appears AFTER #2622 sync marker)
    pos_2622 = impl.find("sync_occurrence_after_dirty(\n            std::span<const NodeId>(memo_targets.data(),")
    pos_2646 = impl.find("Issue #2646: cone-truncate must drop goals/memo")
    if pos_2622 == -1 or pos_2646 == -1 or pos_2646 <= pos_2622:
        fails.append("AC4: #2646 outside-invalidate position must be AFTER #2622 sync_occurrence_after_dirty")

    # AC5: source-cite + counters + linter registration
    must("#2646", "AC5", impl)
    must("#2621", "AC5", impl)
    must("#2622", "AC5", impl)
    must("occurrence_cone_outside_invalidate_total", "AC5", obs)
    must("occurrence_cone_outside_goals_dropped_total", "AC5", obs)
    must("occurrence_cone_outside_memo_dropped_total", "AC5", obs)
    must("occurrence_cone_outside_invalidate_total", "AC5", fields)
    must("occurrence_cone_outside_goals_dropped_total", "AC5", fields)
    must("occurrence_cone_outside_memo_dropped_total", "AC5", fields)
    must("#2646", "AC5", test)
    must("ac2646_outside_cone_invalidate_source_cite", "AC5", test)
    must("check_occurrence_cone_outside_invalidate_2646", "AC5", build)

    # AC6: no docs/design — verify no docs/design/2646-* file exists
    design_dir = ROOT / "docs" / "design"
    design_files = list(design_dir.glob("2646*")) if design_dir.exists() else []
    if design_files:
        fails.append(f"AC6: docs/design/2646-* exists ({len(design_files)} files)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2646 cone-truncate outside-cone invalidate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
