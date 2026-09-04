#!/usr/bin/env python3
"""Issue #3491: production unique cow_set / exclusive with_set zero RMW.

#2058 / #2140 / #3429 unique in-place + shared TLS-then-heap. Residual:
cow_set / exclusive with_set still fetch_add process-wide atomics on the
allocation-free unique path.

Contract:
  AC1  production NDEBUG unique cow_set / exclusive with_set: AURA_PCV_NOTE
       (no-op under AURA_PRODUCTION_PACK+NDEBUG); unique windows have no
       raw fetch_add
  AC2  shared-but-not-stale still tls_pcv_acquire then heap_pcv_allocate;
       Snapshot / SafePCVSpan still COW; locked move-out pattern unchanged
  AC3  Soft / unit: existing counters still named (cow_set_total,
       unique_inplace_total, tls_scratch_*)
  AC4  extend test_pcv_unique_hotpath; linter AFTER #3429; no invent
  AC5  no new query key / metric field; sizeof(PcvHotpathMetrics)==136

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/core/persistent_child_vector.hh")
    ast = _read("src/core/ast.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    t = _read("tests/core/test_pcv_unique_hotpath.cpp")
    tls = _read("tests/core/test_pcv_tls_scratch.cpp")
    bench = _read("tests/bench/cycle221_pcv_bench.cpp")
    build = _read("build.py")
    lint3429 = _read("scripts/coverage/checks/check_pcv_shared_cow_tls_3429.py")

    must("kPcvUniqueZeroAtomicIssue = 3491", "AC1 stamp", hh)
    must("#define AURA_PCV_NOTE", "AC1 note macro", hh)
    must("AURA_PRODUCTION_PACK", "AC1 production pack", hh)
    must("AURA_PCV_METRICS", "AC1 metrics override", hh)
    must("((void)0)", "AC1 production no-op", hh)

    cs = hh.find("void cow_set(")
    cp = hh.find("void cow_push_back(")
    cow = hh[cs:cp] if cs >= 0 and cp > cs else ""
    must("AURA_PCV_NOTE(cow_set_total)", "AC1 cow_set_total", cow)
    must("AURA_PCV_NOTE(unique_inplace_total)", "AC1 unique_inplace", cow)
    if ".fetch_add(" in cow:
        fails.append("AC1: unique cow_set still has raw fetch_add")

    ws = hh.find("PersistentChildVector with_set(")
    wend = hh.find("void cow_set(", ws) if ws >= 0 else -1
    wwin = hh[ws:wend] if ws >= 0 and wend > ws else ""
    ex = wwin.find("data_.use_count() == 1")
    sh = wwin.find("tls_pcv_acquire")
    exclusive = wwin[ex:sh] if ex >= 0 and sh > ex else ""
    must("AURA_PCV_NOTE(unique_inplace_total)", "AC1 exclusive unique_inplace", exclusive)
    must("AURA_PCV_NOTE(with_set_exclusive_total)", "AC1 exclusive with_set", exclusive)
    if ".fetch_add(" in exclusive:
        fails.append("AC1: exclusive with_set still has raw fetch_add")
    must("3491 AC1", "AC1 test", t)

    must("tls_pcv_acquire", "AC2 TLS", wwin)
    must("heap_pcv_allocate", "AC2 heap", wwin)
    tls_pos = wwin.find("tls_pcv_acquire")
    heap_pos = wwin.find("heap_pcv_allocate")
    if not (0 <= tls_pos < heap_pos):
        fails.append("AC2: with_set must call tls_pcv_acquire before heap_pcv_allocate")
    must("*this = with_set", "AC2 cow_set delegates", cow)
    must("auto kids = std::move(children_[id]);", "AC2/AC5 locked move-out", ast)
    must("kids.cow_set(i, new_child);", "AC5 cow_set after move", ast)
    must("children_[id] = std::move(kids);", "AC5 move back", ast)
    must("3491 AC2", "AC2 test", t)
    must("check_pcv_shared_cow_tls_3429", "AC2 3429 linter kept", build)
    must("3429", "AC2 3429 linter body", lint3429)

    must("cow_set_total", "AC3 cow_set_total", hh)
    must("unique_inplace_total", "AC3 unique_inplace", hh)
    must("tls_scratch_hit_total", "AC3 tls hit", hh)
    must("tls_scratch_miss_total", "AC3 tls miss", hh)
    must("3491 AC3", "AC3 test", t)
    must("unique_inplace_total", "AC3 unique_hotpath still asserts", t)
    must("tls_scratch_hit_total", "AC3 tls_scratch still asserts", tls)

    must("check_pcv_unique_zero_atomic_3491", "AC4 build.py", build)
    must("3491 AC4", "AC4 test", t)
    must("cycle221_pcv_bench", "AC4 bench stays", bench)
    prev = build.find("check_pcv_shared_cow_tls_3429")
    ours = build.find("check_pcv_unique_zero_atomic_3491")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3429")
    if (ROOT / "tests" / "core" / "test_issue_3491.cpp").is_file():
        fails.append("AC4: tests/core/test_issue_3491.cpp present")
    if (ROOT / "tests" / "issues" / "test_issue_3491.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3491.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3491-*")):
            fails.append(f"AC4: docs/design/{f.name} present")

    must("sizeof(PcvHotpathMetrics) == 136", "AC5 metrics size", hh)
    must("unique-inplace-total", "AC5 reuse unique-inplace query", q)
    must("with-set-exclusive-total", "AC5 reuse exclusive query", q)
    must("3491 AC5", "AC5 test", t)
    if "schema-3491" in q:
        fails.append("AC5: new schema-3491 query key")
    if "g_3491_" in hh or "g_3491_" in ast:
        fails.append("AC5: new g_3491_* counter")

    if fails:
        print("FAIL #3491 pcv_unique_zero_atomic:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3491 pcv_unique_zero_atomic: unique path zero RMW under production NDEBUG")
    return 0


if __name__ == "__main__":
    sys.exit(main())
