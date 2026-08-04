#!/usr/bin/env python3
"""Issue #2476: partial_recompile single-pass eviction via invalidate_prefix only.

Contract:
  AC1 no invalidate(name) in partial_recompile
  AC2 invalidate_prefix(name) retained + #2476 cite
  AC3 invalidate_prefix covers bare + name#*
  AC4 no sequential invalidate+prefix pair
  AC5 gate wiring

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    jit = _read("src/compiler/aura_jit.cpp")
    test = _read("tests/compiler/test_partial_recompile_single_evict.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    idx = jit.find("bool AuraJIT::partial_recompile")
    body = jit[idx : idx + 1800] if idx >= 0 else ""
    pidx = jit.find("void AuraJIT::invalidate_prefix")
    prefix = jit[pidx : pidx + 1500] if pidx >= 0 else ""

    must("Issue #2476", "AC1", body)
    must_not("invalidate(name);", "AC1", body)  # call site only
    must("invalidate_prefix(name)", "AC1", body)

    must("single pass", "AC2", body)
    must("2476 AC2", "AC2", test)

    must("it->first == p", "AC3", prefix)
    must("p_hash", "AC3", prefix)
    must("fn_trackers_", "AC3", prefix)

    must_not("invalidate(name);\n    invalidate_prefix(name)", "AC4", body)
    must("Issue #2476", "AC4", jit)

    must("check_partial_recompile_single_evict_2476", "gate", build)
    must("cmd_partial_recompile_single_evict_coverage", "gate", build)
    must("test_partial_recompile_single_evict", "gate", cmake)
    must("2476 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: partial_recompile single-pass eviction #2476 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
