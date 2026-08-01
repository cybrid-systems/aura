#!/usr/bin/env python3
"""Issue #2521: production default-on PCV TLS freelist.

Contract:
  AC1 production default ON; AURA_PCV_TLS=0 forces off
  AC2 exclusive stress TLS hits + lower cow_alloc
  AC3 cross-thread no recycle (steal-safe)
  AC4 alloc-count tests force override off
  AC5 schema-2521 + hit/miss/recycle query + gate

Exit 0 = all rows satisfied.
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

    hh = _read("src/core/persistent_child_vector.hh")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/core/test_pcv_tls_default_on_2521.cpp")
    t2058 = _read("tests/core/test_pcv_unique_hotpath_2058.cpp")
    t2140 = _read("tests/core/test_pcv_exclusive_with_set_2140.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2521", "AC1", hh)
    must("production default ON", "AC1", hh)
    must("AURA_PCV_TLS", "AC1", hh)
    must("e[0] == '0'", "AC1", hh)
    must("kPcvTlsDefaultOnIssue", "AC1", hh)
    must("return true", "AC1", hh)
    must("ac1_default_on", "AC1", test)

    # AC2
    must("tls_scratch_hit_total", "AC2", hh)
    must("make_from_tls_or_new", "AC2", hh)
    must("ac2_exclusive_stress", "AC2", test)

    # AC3
    must("another thread", "AC3", hh)
    must("ac3_steal_policy", "AC3", test)

    # AC4
    must("set_pcv_tls_scratch_for_test(false)", "AC4", t2058)
    must("set_pcv_tls_scratch_for_test(false)", "AC4", t2140)
    must("ac4_alloc_count_override", "AC4", test)

    # AC5
    must("schema-2521", "AC5", q)
    must("tls-scratch-production-default-on", "AC5", q)
    must("tls-scratch-hit-total", "AC5", q)
    must("tls-scratch-miss-total", "AC5", q)
    must("tls-scratch-recycle-total", "AC5", q)
    must("test_pcv_tls_default_on_2521", "AC5", cmake)
    must("check_pcv_tls_default_on_2521", "AC5", build)
    must("cmd_pcv_tls_default_on_coverage", "AC5", build)
    must("ac5_query", "AC5", test)

    # Retain 2406 foundation
    must("kPcvTlsScratchIssue", "retain", hh)
    must("schema-2406", "retain", q)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2521 PCV TLS production default ON — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
