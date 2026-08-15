#!/usr/bin/env python3
"""Issue #3069: abort force_ir_cache_dirty_after_abort generation fence.

Bump abort_force_generation_ before walking ir_cache_v2_. Concurrent
lookup_define_v2 must not return a clean hit mid-loop. Soft/Off: fence
only armed when abort runs (never-aborted lookup is one acquire of 0).

Contract:
  AC1 Concurrent lookup during abort force never returns clean hit
  AC2 After abort: dirty + zero stamps + map consistent; metric bumps
  AC3 Success-path store/restamp acks the fence (clean hit)
  AC4 extend test_mutation_rollback_coverage + stamp restamp;
      linter; no docs/design/; no test_issue_3069; no new query keys

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

    svc = _read("src/compiler/service.ixx")
    pure = _read("src/compiler/ir_cache_pure.ixx")
    q = read_query_prims() + _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    t = _read("tests/compiler/test_mutation_rollback_coverage.cpp")
    stamp = _read("tests/compiler/test_cache_stamp_restamp_contract.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3069", "AC1 service", svc)
    must("abort_force_generation_", "AC1 gen", svc)
    must("abort_force_in_progress_", "AC1 in-progress", svc)
    must("fetch_add(1, std::memory_order_release)", "AC1 bump before walk", svc)
    must("test_abort_force_generation_fence_3069", "AC1 test", t)
    must("no clean hit during abort force", "AC1 soak", t)

    # AC2
    must("abort_force_generation = gen", "AC2 ack seen", svc)
    must("abort_ir_cache_force_dirty_total", "AC2 existing metric", t)
    must("map consistent", "AC2 map", t)

    # AC3
    must("kRelowerAbortForce", "AC3 reason bit", pure)
    must("abort_force_generation", "AC3 stamp field", pure)
    must("success-path restamp acks", "AC3 live restamp", svc)
    must("kRelowerAbortForce", "AC3 stamp test", stamp)
    must("3069 AC3: success-path store is clean hit", "AC3 store", t)

    # AC4
    must("Issue #3069", "AC4 pure cite", pure)
    must("check_abort_force_generation_fence_3069", "AC4 build", build)
    must("cmd_abort_force_generation_fence_3069", "AC4 cmd", build)
    if "schema-3069" in q:
        fails.append("AC4: new query key schema-3069 (forbidden)")
    if (ROOT / "tests" / "compiler" / "test_issue_3069.cpp").is_file():
        fails.append("AC4: test_issue_3069.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*3069*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3069 abort-force generation fence — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
