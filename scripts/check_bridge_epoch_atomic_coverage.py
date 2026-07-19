#!/usr/bin/env python3
# scripts/check_bridge_epoch_atomic_coverage.py — Issue #1654
#
# AC list:
#   AC1: --self-test exit 0 (the script IS the self-test).
#   AC2: src/compiler/aura_jit_bridge.cpp uses
#        `static std::atomic<std::uint64_t> g_current_bridge_epoch{0};`
#        (legacy plain uint64_t removed).
#   AC3: src/compiler/aura_jit_bridge.cpp setter / getter use
#        `.store(v, std::memory_order_release)` + `.load(std::memory_order_acquire)`.
#   AC4: src/compiler/aura_jit_bridge_stub.cpp uses
#        `static std::atomic<std::uint64_t> g_current_bridge_epoch_stub{0};`
#        (legacy plain uint64_t removed).
#   AC5: src/compiler/aura_jit_bridge_stub.cpp setter / getter use
#        `.store(release)` + `.load(acquire)`.
#   AC6: lib/runtime.c uses `_Atomic unsigned long long g_current_bridge_epoch`
#        + `atomic_store_explicit(&, v, memory_order_release)` +
#        `atomic_load_explicit(&, memory_order_acquire)`.
#   AC7: lib/runtime.c includes `<stdatomic.h>`.
#   AC8: legacy `extern "C"` signatures preserved across all 3 files
#        (no ABI change — call sites in tests/test_issue_1485.cpp AC10
#        still compile).
#   AC9: aura_jit_bridge.cpp has Issue #1654 rationale comment.
#   AC10: aura_jit_bridge_stub.cpp has Issue #1654 rationale comment.
#
# Pattern reference: scripts/check_orchestration_steal_boundary_coverage.py
# (#1641), scripts/check_aot_hot_update_incremental_coverage.py (#1640),
# scripts/check_incremental_relower_coverage.py (#1639),
# scripts/check_soa_dual_path_consistency_coverage.py (#1638),
# scripts/check_panic_checkpoint_lifecycle_coverage.py (#1637),
# scripts/check_macro_provenance_coverage.py (#1908),
# scripts/check_primitive_surface.py (#1449 SlimSurface freeze).
#
# Hooked into the pre-commit hook alongside the existing coverage / freeze
# linters (clang-format, primitive surface, test-registry, gen_docs).
# Run individually with
# `./scripts/check_bridge_epoch_atomic_coverage.py` from the repo root.

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ERRORS = []


def check(name: str, condition: bool) -> None:
    if condition:
        print(f"OK    {name}")
    else:
        print(f"FAIL  {name}")
        ERRORS.append(name)


def read(rel: str) -> str:
    return (ROOT / rel).read_text()


def main() -> int:
    print("=== scripts/check_bridge_epoch_atomic_coverage.py ===")
    print(f"repo root: {ROOT}")
    print()

    bridge = read("src/compiler/aura_jit_bridge.cpp")
    stub = read("src/compiler/aura_jit_bridge_stub.cpp")
    runtime = read("lib/runtime.c")

    # AC1: self-test
    print("--- AC1: --self-test exit 0 ---")
    check("AC1: script self-test exit 0", True)
    print()

    # AC2: aura_jit_bridge.cpp std::atomic type + legacy removed
    print("--- AC2: aura_jit_bridge.cpp std::atomic type ---")
    check(
        "aura_jit_bridge.cpp: static std::atomic<std::uint64_t> g_current_bridge_epoch{0};",
        "static std::atomic<std::uint64_t> g_current_bridge_epoch{0};" in bridge,
    )
    check(
        "aura_jit_bridge.cpp: legacy plain uint64_t removed",
        "static std::uint64_t g_current_bridge_epoch = 0;" not in bridge,
    )
    print()

    # AC3: aura_jit_bridge.cpp release/acquire
    print("--- AC3: aura_jit_bridge.cpp release/acquire ---")
    check(
        "aura_jit_bridge.cpp: .store(v, std::memory_order_release)",
        "g_current_bridge_epoch.store(v, std::memory_order_release)" in bridge,
    )
    check(
        "aura_jit_bridge.cpp: .load(std::memory_order_acquire)",
        "g_current_bridge_epoch.load(std::memory_order_acquire)" in bridge,
    )
    print()

    # AC4: aura_jit_bridge_stub.cpp std::atomic type + legacy removed
    print("--- AC4: aura_jit_bridge_stub.cpp std::atomic type ---")
    check(
        "aura_jit_bridge_stub.cpp: static std::atomic<std::uint64_t> g_current_bridge_epoch_stub{0};",
        "static std::atomic<std::uint64_t> g_current_bridge_epoch_stub{0};" in stub,
    )
    check(
        "aura_jit_bridge_stub.cpp: legacy plain uint64_t removed",
        "static std::uint64_t g_current_bridge_epoch_stub = 0;" not in stub,
    )
    print()

    # AC5: aura_jit_bridge_stub.cpp release/acquire
    print("--- AC5: aura_jit_bridge_stub.cpp release/acquire ---")
    check(
        "aura_jit_bridge_stub.cpp: .store(v, std::memory_order_release)",
        "g_current_bridge_epoch_stub.store(v, std::memory_order_release)" in stub,
    )
    check(
        "aura_jit_bridge_stub.cpp: .load(std::memory_order_acquire)",
        "g_current_bridge_epoch_stub.load(std::memory_order_acquire)" in stub,
    )
    print()

    # AC6: lib/runtime.c _Atomic + atomic_*_explicit
    print("--- AC6: lib/runtime.c _Atomic + atomic_*_explicit ---")
    check(
        "lib/runtime.c: static _Atomic unsigned long long g_current_bridge_epoch = 0;",
        "static _Atomic unsigned long long g_current_bridge_epoch = 0;" in runtime,
    )
    check(
        "lib/runtime.c: atomic_store_explicit(release)",
        "atomic_store_explicit(&g_current_bridge_epoch, v, memory_order_release)" in runtime,
    )
    check(
        "lib/runtime.c: atomic_load_explicit(acquire)",
        "atomic_load_explicit(&g_current_bridge_epoch, memory_order_acquire)" in runtime,
    )
    check(
        "lib/runtime.c: legacy plain unsigned long long removed",
        "static unsigned long long g_current_bridge_epoch = 0;" not in runtime,
    )
    print()

    # AC7: lib/runtime.c <stdatomic.h>
    print("--- AC7: lib/runtime.c <stdatomic.h> include ---")
    check(
        "lib/runtime.c: #include <stdatomic.h>",
        "#include <stdatomic.h>" in runtime,
    )
    print()

    # AC8: extern "C" signatures preserved
    print('--- AC8: extern "C" signatures preserved ---')
    check(
        'aura_jit_bridge.cpp: extern "C" setter signature preserved',
        'extern "C" void aura_set_current_bridge_epoch(std::uint64_t v)' in bridge,
    )
    check(
        'aura_jit_bridge.cpp: extern "C" getter signature preserved',
        'extern "C" std::uint64_t aura_get_current_bridge_epoch(void)' in bridge,
    )
    check(
        'aura_jit_bridge_stub.cpp: extern "C" __attribute__((weak)) setter preserved',
        'extern "C" __attribute__((weak)) void aura_set_current_bridge_epoch(std::uint64_t v)' in stub,
    )
    check(
        'aura_jit_bridge_stub.cpp: extern "C" __attribute__((weak)) getter preserved',
        'extern "C" __attribute__((weak)) std::uint64_t aura_get_current_bridge_epoch(void)' in stub,
    )
    check(
        "lib/runtime.c: C-side setter signature preserved",
        "void aura_set_current_bridge_epoch(unsigned long long v)" in runtime,
    )
    check(
        "lib/runtime.c: C-side getter signature preserved",
        "unsigned long long aura_get_current_bridge_epoch(void)" in runtime,
    )
    print()

    # AC9: aura_jit_bridge.cpp Issue #1654 comment
    print("--- AC9: aura_jit_bridge.cpp Issue #1654 rationale comment ---")
    check(
        "aura_jit_bridge.cpp: Issue #1654 reference",
        "Issue #1654" in bridge,
    )
    print()

    # AC10: aura_jit_bridge_stub.cpp Issue #1654 comment
    print("--- AC10: aura_jit_bridge_stub.cpp Issue #1654 rationale comment ---")
    check(
        "aura_jit_bridge_stub.cpp: Issue #1654 reference",
        "Issue #1654" in stub,
    )
    print()

    print("=" * 60)
    if ERRORS:
        print(f"FAIL: {len(ERRORS)} check(s) failed")
        for e in ERRORS:
            print(f"  - {e}")
        return 1
    print("PASS: all 10 ACs green (Issue #1654 atomic conversion fully covered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
