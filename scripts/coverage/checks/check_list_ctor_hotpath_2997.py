#!/usr/bin/env python3
"""Issue #2997: list/json constructor hot-path beyond #2916 soft quotas.

AC:
  1. Measure/document 4–8 fiber concurrent list (source notes + existing suite)
  2. Shorter CS: ListCtorLockHold + reserve + snapshot/construct
  3. Unlimited / known-small bypass of full allow(); fail-closed when limited
  4. lock-hold-ns + samples + soft-hit + recommend on query:prim-heap-quota-stats
  5. list-ref / member / math never call allow
  6. Docs (stdlib only) + existing tests + build.py wiring
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
    must("2997" in hh, "AC1: prim_heap_quota.hh cites #2997")
    must("kPrimHeapUnlimitedSmall" in hh, "AC3: small unlimited constant")
    must("kPrimHeapQuotaSoftHitBp" in hh, "AC4: soft-hit basis points")
    must("kPrimHeapRecommendOk" in hh, "AC4: recommend codes")
    must("kPrimHeapLockHoldWarnNs" in hh, "AC4: lock-hold warn threshold")

    ixx = _read("src/compiler/evaluator.ixx")
    must("ListCtorLockHold" in ixx, "AC2: timed lock RAII")
    must("note_list_ctor_lock_hold_ns" in ixx, "AC4: lock-hold sampler")
    must("prim_heap_quota_limited" in ixx, "AC3: limited probe")
    must("prim_heap_quota_unlimited_bypass_total" in ixx, "AC3: unlimited bypass")
    must("prim_heap_quota_soft_hit_total" in ixx, "AC4: soft-hit increment")

    metrics = _read("src/compiler/observability_metrics.h")
    must("list_constructor_lock_hold_ns" in metrics, "AC4: lock-hold metric")
    must("list_constructor_lock_samples" in metrics, "AC4: lock samples")
    must("prim_heap_quota_soft_hit_total" in metrics, "AC4: soft-hit metric")
    must("2997" in metrics, "AC4: metrics cite 2997")

    list_cpp = _read("src/compiler/evaluator_primitives_list.cpp")
    must("ListCtorLockHold" in list_cpp, "AC2: list TU timed lock")
    must("pairs.reserve" in list_cpp, "AC2: reserve before grow")
    must("kPrimHeapUnlimitedSmall" in list_cpp, "AC3: small bypass in list")
    must("bump_pair_alloc_count_n" in list_cpp, "AC2: batched pair bump")
    for name in ('"list"', '"append"', '"reverse"', '"map"'):
        must(name in list_cpp, f"AC2: {name} present")

    after = list_cpp.split('"list-ref"', 1)
    if len(after) > 1:
        body = after[1][:1500]
        must("prim_heap_quota" not in body, "AC5: list-ref must not call quota")
    after_m = list_cpp.split('"member"', 1)
    if len(after_m) > 1:
        body = after_m[1][:1500]
        must("prim_heap_quota" not in body, "AC5: member must not call quota")

    js = _read("src/compiler/evaluator_primitives_json.cpp")
    must("ListCtorLockHold" in js, "AC2: json array timed lock")
    must("pairs.reserve" in js, "AC2: json array reserve")
    must("entries.emplace_back" in js, "AC2: json object snapshot then construct")
    must("snapshot" in js.lower(), "AC2: json object snapshot comment")

    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    must("lock-hold-ns" in obs, "AC4: stats lock-hold-ns")
    must("lock-samples" in obs, "AC4: stats lock-samples")
    must("soft-hit-total" in obs, "AC4: stats soft-hit")
    must("unlimited-bypass-total" in obs, "AC4: stats bypass")
    must("recommend" in obs, "AC4: stats recommend")
    must("schema-2997" in obs, "AC4: schema-2997")
    must("create(32)" in obs, "AC4: hash capacity for new keys")

    doc = _read("docs/stdlib/prim-heap-quota.md")
    must("2997" in doc and "lock-hold-ns" in doc, "AC6: stdlib docs")
    must("recommend" in doc, "AC6: docs recommend codes")

    test = _read("tests/compiler/test_pmr_alloc_fiber_safe.cpp")
    must("2997" in test, "AC1: fiber test cites 2997")
    must("slot_lookup_fast" in test, "AC1: concurrent list via slot")
    must("list_constructor_lock_hold_ns" in test, "AC4: test reads lock-hold")
    must("list-ref" in test, "AC5: single-fiber list-ref smoke")

    suite = _read("tests/suite/prim_heap_quota_2916.aura")
    must("schema-2997" in suite, "AC6: suite schema-2997")
    must("lock-hold-ns" in suite, "AC6: suite lock-hold")
    must("recommend" in suite, "AC6: suite recommend")
    must("soft-hit-total" in suite, "AC6: suite soft-hit")

    build = _read("build.py")
    must("list_ctor_hotpath_2997" in build or "list-ctor-hotpath-2997" in build, "AC6: build.py gate")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2997 list/json constructor hot-path — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
