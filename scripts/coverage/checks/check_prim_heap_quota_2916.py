#!/usr/bin/env python3
"""Issue #2916: multi-fiber hot-path prim heap soft quotas.

AC:
  1. prim_heap_quota.hh + Evaluator allow/set APIs present
  2. High-freq constructors participate (list/map/append, json, string, vector)
  3. Agent surfaces: resource:quota-set pairs|strings|vectors + stats
  4. Docs + suite + build.py wiring
  5. list-ref path does not call prim_heap_quota_allow (no single-fiber hit)
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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    hh = _read("src/compiler/prim_heap_quota.hh")
    must("Issue #2916" in hh, "AC1: prim_heap_quota.hh missing or uncited")
    must("PrimHeapDim" in hh, "AC1: PrimHeapDim")
    must("prim_heap_quota_exceeded_msg" in hh, "AC1: error msg helper")

    ixx = _read("src/compiler/evaluator.ixx")
    must("prim_heap_quota_allow" in ixx, "AC1: Evaluator::prim_heap_quota_allow")
    must("set_prim_heap_quota" in ixx, "AC1: set_prim_heap_quota")
    must("prim_heap_quota_pairs_" in ixx, "AC1: pairs limit field")

    metrics = _read("src/compiler/observability_metrics.h")
    must("prim_heap_quota_checks_total" in metrics, "AC1: metrics checks")
    must("prim_heap_quota_rejects_total" in metrics, "AC1: metrics rejects")
    must("2916" in metrics, "AC1: metrics cite 2916")

    list_cpp = _read("src/compiler/evaluator_primitives_list.cpp")
    must("prim_heap_quota_allow" in list_cpp, "AC2: list TU uses allow helper")
    must("PrimHeapDim::Pairs" in list_cpp, "AC2: pairs dim in list")
    must('add("list"' in list_cpp, "AC2: list registered")
    must('add("map"' in list_cpp, "AC2: map registered")
    must('add("append"' in list_cpp, "AC2: append registered")
    must('add("list-ref"' in list_cpp, "AC5: list-ref present")
    # list-ref lambda body must not consult the soft quota
    after = list_cpp.split('add("list-ref"', 1)
    if len(after) > 1:
        body = after[1][:1500]
        must("prim_heap_quota" not in body, "AC5: list-ref must not call prim_heap_quota")

    pair = _read("src/compiler/evaluator_primitives_pair.cpp")
    must("string-append" in pair and "PrimHeapDim::Strings" in pair, "AC2: string-append quota")
    must("cons" in pair and "PrimHeapDim::Pairs" in pair, "AC2: cons quota")

    js = _read("src/compiler/evaluator_primitives_json.cpp")
    must("PrimHeapDim::Strings" in js, "AC2: json string quota")
    must("PrimHeapDim::Pairs" in js, "AC2: json pairs quota")
    must("Evaluator& ev" in js or "&ev" in js, "AC2: json has Evaluator")

    vec = _read("src/compiler/evaluator_primitives_vector.cpp")
    must("PrimHeapDim::Vectors" in vec, "AC2: vector quota")

    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    must('kind == "pairs"' in obs, "AC3: resource:quota-set pairs")
    must('kind == "strings"' in obs, "AC3: resource:quota-set strings")
    must('kind == "vectors"' in obs, "AC3: resource:quota-set vectors")
    must("query:prim-heap-quota-stats" in obs, "AC3: stats surface")
    must("kPrimHeapQuotaSchema" in obs or "2916" in obs, "AC3: schema 2916")

    cat = _read("src/compiler/evaluator_primitives_observability.cpp")
    must("query:prim-heap-quota-stats" in cat, "AC3: stats catalog entry")

    doc = _read("docs/stdlib/prim-heap-quota.md")
    must("2916" in doc and "resource:quota-set" in doc, "AC4: docs")

    suite = _read("tests/suite/prim_heap_quota_2916.aura")
    must("2916" in suite and "resource:quota-set" in suite, "AC4: suite")
    must("query:prim-heap-quota-stats" in suite, "AC4: suite stats")

    build = _read("build.py")
    must("prim_heap_quota_2916" in build or "prim-heap-quota-2916" in build, "AC4: build.py gate")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2916 prim-heap soft quotas — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
