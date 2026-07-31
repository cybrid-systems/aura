#!/usr/bin/env python3
"""Issue #2406: optional TLS freelist for exclusive PCV unique-inplace.

Contract:
  AC1 default OFF — identical path (AURA_PCV_TLS / test override)
  AC2 opt-in reduces cow_alloc under exclusive stress (TLS hit skips cow_alloc)
  AC3 SafePCVSpan semantics unchanged
  AC4 metrics + schema-2406 on query:pcv-hotpath-stats
  AC5 multi-thread stress + build gate

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
    test = _read("tests/core/test_pcv_tls_scratch_2406.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 default off
    must("Issue #2406", "AC1", hh)
    must("AURA_PCV_TLS", "AC1", hh)
    must("pcv_tls_scratch_enabled", "AC1", hh)
    must("pcv_tls_scratch_active", "AC1", hh)
    must("2406 AC1", "AC1", test)

    # AC2 TLS freelist
    must("tls_scratch_hit_total", "AC2", hh)
    must("make_from_tls_or_new", "AC2", hh)
    must("note_pcv_alloc", "AC2", hh)
    must("kTlsMaxElems", "AC2", hh)
    must("2406 AC2", "AC2", test)

    # AC3 SafePCVSpan
    must("SafePCVSpan", "AC3", hh)
    must("2406 AC3", "AC3", test)

    # AC4 query
    must("query:pcv-hotpath-stats", "AC4", q)
    must("schema-2406", "AC4", q)
    must("tls-scratch-wired", "AC4", q)
    must("tls-scratch-hit-total", "AC4", q)
    must("kPcvTlsScratchIssue", "AC4", hh)
    must("2406 AC4", "AC4", test)

    # AC5
    must("2406 AC5", "AC5", test)
    must("check_pcv_tls_scratch_2406", "AC5", build)
    must("cmd_pcv_tls_scratch_coverage", "AC5", build)
    must("test_pcv_tls_scratch_2406", "AC5", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: pcv TLS scratch #2406 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
