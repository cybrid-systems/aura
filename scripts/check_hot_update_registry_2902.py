#!/usr/bin/env python3
# scripts/check_hot_update_registry_2902.py — Issue #2902 source-cite gate.
# CI ubsan-smoke failed on hot_update_registry.cpp:2613
#   error: 'partial_relower_threshold_forced_atomic' is not a member of 'aura::compiler'
# Root cause: hot_update_registry.cpp is a non-module .cpp that references
#   aura::compiler::partial_relower_threshold_forced_atomic (defined in
#   src/compiler/ir_cache_pure.ixx:886) WITHOUT importing the module.
# It also calls aura_clear_partial_relower_threshold_force() at line 1409
#   ~1200 lines before its extern "C" definition at line 2612, so the
#   call site lacks a forward declaration. ASAN/ubsan builds expose this
#   (production NDEBUG takes a different path so it was masked until CI
#   hardened the gates).
#
# This linter enforces BOTH preconditions going forward:
#   1. import aura.compiler.ir_cache_pure; present in hot_update_registry.cpp
#   2. extern "C" void aura_clear_partial_relower_threshold_force(void);
#      forward-declared before the line 1409 call site
#
# Usage:
#   python3 scripts/check_hot_update_registry_2902.py --strict
#   python3 scripts/check_hot_update_registry_2902.py --self-test

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TARGET = REPO_ROOT / "src" / "compiler" / "hot_update_registry.cpp"

IMPORT_PATTERN = re.compile(r"^\s*import\s+aura\.compiler\.ir_cache_pure\s*;", re.MULTILINE)
FORWARD_DECL_PATTERN = re.compile(
    r'extern\s+"C"\s+void\s+aura_clear_partial_relower_threshold_force\s*\(\s*void\s*\)\s*;',
    re.MULTILINE,
)
CALL_SITE_PATTERN = re.compile(r"\baura_clear_partial_relower_threshold_force\s*\(\s*\)\s*;")
ATOMIC_ACCESSOR_PATTERN = re.compile(r"aura::compiler::partial_relower_threshold_forced_atomic\s*\(")


def _read_text(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def check_file(path: Path = TARGET, *, strict: bool = True) -> list[str]:
    failures: list[str] = []
    if not path.exists():
        failures.append(f"{path}: file not found")
        return failures
    text = _read_text(path)
    if not text:
        failures.append(f"{path}: empty file")
        return failures

    # 1. import statement
    if not IMPORT_PATTERN.search(text):
        failures.append(
            f"{path}: missing 'import aura.compiler.ir_cache_pure;' "
            "(required for partial_relower_threshold_forced_atomic visibility)"
        )

    # 2. forward decl for the C-linkage function
    if not FORWARD_DECL_PATTERN.search(text):
        failures.append(
            f"{path}: missing forward declaration "
            "'extern \"C\" void aura_clear_partial_relower_threshold_force(void);' "
            "(required because call site at line ~1409 is before the definition at line ~2612)"
        )

    # 3. forward decl must appear BEFORE the call site
    fwd_match = FORWARD_DECL_PATTERN.search(text)
    call_match = CALL_SITE_PATTERN.search(text)
    if fwd_match and call_match and fwd_match.start() >= call_match.start():
        failures.append(
            f"{path}: forward declaration for aura_clear_partial_relower_threshold_force "
            f"appears AFTER the call site (fwd@{fwd_match.start()} call@{call_match.start()}); "
            "move the forward decl above the call site"
        )

    # 4. atomic accessor reference present (sanity: if this file references
    #    the accessor, the import must be present — already covered by #1).
    #    This is informational only.
    if not ATOMIC_ACCESSOR_PATTERN.search(text) and strict:
        # Not a hard failure — the file may not currently reference it,
        # but if it does in the future, the import is required.
        pass

    return failures


def _self_test() -> int:
    """Validate the linter regex / structure against fixture text."""
    fixture_ok = """
#include "compiler/foo.h"
import aura.compiler.ir_cache_pure; // Issue #2902

extern "C" void aura_clear_partial_relower_threshold_force(void);

namespace aura::compiler {
void some_func() {
    aura_clear_partial_relower_threshold_force();
    auto& x = partial_relower_threshold_forced_atomic();
}
}
"""
    fixture_missing_import = """
extern "C" void aura_clear_partial_relower_threshold_force(void);
namespace aura::compiler {
void some_func() { aura_clear_partial_relower_threshold_force(); }
}
"""
    fixture_missing_fwd = """
import aura.compiler.ir_cache_pure;
namespace aura::compiler {
void some_func() { aura_clear_partial_relower_threshold_force(); }
}
"""
    fixture_fwd_after_call = """
import aura.compiler.ir_cache_pure;
namespace aura::compiler {
void some_func() { aura_clear_partial_relower_threshold_force(); }
}
extern "C" void aura_clear_partial_relower_threshold_force(void) {}
"""

    class _T:
        def __init__(self, t):
            self._t = t

        def read_text(self, encoding="utf-8"):
            return self._t

        def exists(self):
            return True

    fails: list[str] = []
    for label, txt, should_fail in [
        ("ok", fixture_ok, False),
        ("missing_import", fixture_missing_import, True),
        ("missing_fwd", fixture_missing_fwd, True),
        ("fwd_after_call", fixture_fwd_after_call, True),
    ]:
        result = check_file(_T(txt), strict=True)
        has_fail = len(result) > 0
        if has_fail != should_fail:
            fails.append(f"fixture {label}: expected should_fail={should_fail} got has_fail={has_fail} ({result})")
    if fails:
        print("SELF-TEST FAIL:")
        for line in fails:
            print(f"  {line}")
        return 1
    print("SELF-TEST PASS: all required patterns enforced (import + forward-decl)")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Issue #2902 hot_update_registry.cpp forward-decl + import gate")
    parser.add_argument("--strict", action="store_true", default=True)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("targets", nargs="*")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    targets = [Path(t) for t in args.targets] if args.targets else [TARGET]
    failures: list[str] = []
    for t in targets:
        failures.extend(check_file(t, strict=args.strict))

    if failures:
        print("FAIL:")
        for line in failures:
            print(f"  {line}")
        return 1
    print("OK: hot_update_registry.cpp #2902 source-cite gate (import + forward-decl present)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
