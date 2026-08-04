#!/usr/bin/env python3
"""Issue #2475: unused fn_lock removed from AuraJIT::Impl::compile().

Contract:
  AC1 fn_lock gone from compile()
  AC2 comments document global serialize + cache-only mtx
  AC3 shared_lock cache lookup + unique publish retained
  AC4 #2475 cite
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
    test = _read("tests/compiler/test_aura_jit_unused_fn_lock.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    idx = jit.find("ScalarFn compile(const FlatFunction")
    body = jit[idx : idx + 4500] if idx >= 0 else ""

    must("Issue #2475", "AC1", body)
    # Declaration only — comment may still mention "fn_lock" as removed.
    must_not("unique_lock<std::shared_mutex> fn_lock", "AC1", body)
    must_not("fn_lock;", "AC1", body)
    must("compile_mtx_", "AC1", body)

    must_not("Two threads compiling DIFFERENT functions run in parallel", "AC2", body)
    must("fn_compile_mtx_", "AC2", body)
    must("2475 AC2", "AC2", test)

    must("shared_lock", "AC3", body)
    must("compile_fns_", "AC3", body)
    # Publish site elsewhere in file
    must("unique_lock", "AC3", jit)
    must("compile_fns_[std::string(fn.name)]", "AC3", jit)

    must("Issue #2475", "AC4", jit)
    must("2475 AC4", "AC4", test)

    must("check_aura_jit_unused_fn_lock_2475", "gate", build)
    must("cmd_aura_jit_unused_fn_lock_coverage", "gate", build)
    must("test_aura_jit_unused_fn_lock", "gate", cmake)
    must("2475 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: AuraJIT unused fn_lock #2475 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
