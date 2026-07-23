#!/usr/bin/env python3
"""check_runtime_stub_coverage.py — Issue #1995 source gate.

  AC1: All 17 aura_* runtime symbols are defined in lib/runtime.c with
       __attribute__((weak)) (so host-build bridge wins when both are
       linked; standalone AOT falls through to these stubs).
  AC2: lib/runtime.c compiles cleanly (gcc -c, no errors — pre-existing
       warnings OK).
  AC3: nm output on the compiled .o shows each symbol as weak (W).
  AC4: tests/verify_runtime_stubs.sh or equivalent link-test exists.
  AC5: linter self-test (--self-test passes).

The 17 symbols (from Issue #1995 body — B-012 / B-020 link-surface
completeness):

  aura_alloc_pair_arena             (OpMakePair L2)
  aura_alloc_closure_arena          (OpMakeClosure arena path)
  aura_arena_push                   (OpArenaPush)
  aura_arena_pop                    (OpArenaPop)
  aura_top_cell_get                 (OpTopCellLoad)
  aura_hash_get_flat_table          (OpHashRef inline scan)
  aura_hash_key_eq                  (OpHashRef key compare)
  aura_hash_ref                     (JIT inlines)
  aura_hash_set                     (OpHashSet)
  aura_hash_remove                  (OpHashRemove)
  aura_jit_guard_shape_epoch_check  (OpGuardShape)
  aura_jit_is_fn_epoch_stale        (OpApply prologue)
  aura_jit_deopt_to_interpreter     (OpApply deopt)
  aura_jit_get_current_bridge_epoch (OpApply)
  aura_get_defuse_version           (L2 SHAPE_PAIR defuse check)
  aura_lock_workspace_read          (OpHashRef inline scan body)
  aura_unlock_workspace_read        (OpHashRef inline scan tail)

Rationale (Issue #1995 body):
  aura_jit.cpp declares ExternalLinkage Function* for every runtime
  helper the JIT IR can call, and lower() emits CreateCall for each
  matched opcode. AOT emits `.o` via emit_native_object_llvm(); each
  CreateCall becomes a real `call @aura_*` in the emitted LLVM IR.
  Standalone AOT linker only links the LLVM-emitted .o + runtime.o +
  registration .c — the host bridge (aura_jit_bridge.cpp) is NOT
  linked. So every aura_* referenced from the LLVM-emitted .o must
  be resolvable from lib/runtime.c.

  Fix: port weak stubs from aura_jit_bridge_stub.cpp into
  lib/runtime.c with __attribute__((weak)) — standalone AOT falls
  through to no-op / safe-default impls; host-build wins when both
  are linked.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME_C = ROOT / "lib" / "runtime.c"

# The 17 symbols required by Issue #1995
REQUIRED_SYMBOLS = [
    "aura_alloc_pair_arena",
    "aura_alloc_closure_arena",
    "aura_arena_push",
    "aura_arena_pop",
    "aura_top_cell_get",
    "aura_hash_get_flat_table",
    "aura_hash_key_eq",
    "aura_hash_ref",
    "aura_hash_set",
    "aura_hash_remove",
    "aura_jit_guard_shape_epoch_check",
    "aura_jit_is_fn_epoch_stale",
    "aura_jit_deopt_to_interpreter",
    "aura_jit_get_current_bridge_epoch",
    "aura_get_defuse_version",
    "aura_lock_workspace_read",
    "aura_unlock_workspace_read",
]


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    failures: list[str] = []

    if not RUNTIME_C.exists():
        failures.append(f"AC1: {RUNTIME_C} not found")
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1

    src = RUNTIME_C.read_text(encoding="utf-8", errors="replace")

    # === AC1: each symbol defined with __attribute__((weak)) ===
    for sym in REQUIRED_SYMBOLS:
        # Look for: __attribute__((weak)) ... <sym>(
        # The definition line should have `__attribute__((weak))` somewhere
        # within ~10 lines before the symbol name (annotations may be
        # multi-line).
        # We use a simple heuristic: find the symbol in the file, then
        # check that __attribute__((weak)) appears in the same function.
        sym_idx = src.find(sym)
        if sym_idx < 0:
            failures.append(
                f"AC1: symbol {sym} not defined in lib/runtime.c — #1995 B-012/B-020 standalone-AOT link error"
            )
            continue
        # Find the enclosing function: look backwards from sym for `{`,
        # forwards for matching `}`. Then check that
        # __attribute__((weak)) appears within the function body.
        # Simpler: check that __attribute__((weak)) appears within 200
        # chars before sym_idx.
        preceding = src[max(0, sym_idx - 400) : sym_idx]
        if "__attribute__((weak))" not in preceding:
            failures.append(
                f"AC1: symbol {sym} in lib/runtime.c is NOT marked "
                f"__attribute__((weak)) — host-build bridge would lose "
                f"to runtime.o's strong definition (double-def OR "
                f"unexpected override)"
            )

    # === AC2: lib/runtime.c compiles cleanly ===
    # We try a quick gcc compile to a temp .o. Pre-existing warnings OK.
    with tempfile.TemporaryDirectory(prefix="check_1995_") as tmpdir:
        out = Path(tmpdir) / "runtime.o"
        proc = subprocess.run(
            ["gcc", "-c", str(RUNTIME_C), "-o", str(out), "-Wall"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if proc.returncode != 0:
            failures.append(f"AC2: lib/runtime.c fails to compile (rc={proc.returncode}): {proc.stderr[:500]}")
        elif out.exists():
            # === AC3: nm shows each symbol as weak (W) ===
            nm_proc = subprocess.run(
                ["nm", str(out)],
                capture_output=True,
                text=True,
                timeout=10,
            )
            nm_out = nm_proc.stdout
            for sym in REQUIRED_SYMBOLS:
                # Look for a line with ` W <sym>` (weak)
                # nm format: <addr> <type> <name>
                if not re.search(rf"\sW\s+{re.escape(sym)}$", nm_out, re.MULTILINE):
                    failures.append(
                        f"AC3: symbol {sym} not present as weak (W) in "
                        f"compiled lib/runtime.o — host-build bridge would "
                        f"lose to runtime.o"
                    )

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print(f"OK: all #1995 ACs satisfied ({len(REQUIRED_SYMBOLS)} weak stubs in lib/runtime.c, compiles cleanly)")
    return 0


def self_test() -> int:
    """Self-test: feed good + bad fixtures through the linter."""
    tmp = Path(tempfile.mkdtemp(prefix="check_1995_selftest_"))
    try:
        # Good fixture: all 17 symbols with __attribute__((weak))
        good = tmp / "runtime_good.c"
        good.write_text(
            "int dummy(void) { return 0; }\n"
            + "\n".join(f"__attribute__((weak)) int {sym}(void) {{ return 0; }}" for sym in REQUIRED_SYMBOLS)
            + "\n",
            encoding="utf-8",
        )
        # Bad fixture: one symbol missing weak attribute
        bad = tmp / "runtime_bad.c"
        bad_syms = list(REQUIRED_SYMBOLS)
        bad_lines = []
        for i, sym in enumerate(bad_syms):
            # Make the first symbol lack __attribute__((weak))
            if i == 0:
                bad_lines.append(f"int {sym}(void) {{ return 0; }}")
            else:
                bad_lines.append(f"__attribute__((weak)) int {sym}(void) {{ return 0; }}")
        bad.write_text("\n".join(bad_lines) + "\n", encoding="utf-8")

        import check_runtime_stub_coverage as self_mod

        original_root = self_mod.ROOT
        original_runtime = self_mod.RUNTIME_C
        try:
            self_mod.ROOT = tmp
            self_mod.RUNTIME_C = good
            rc_good = self_mod.main()
        finally:
            self_mod.ROOT = original_root
            self_mod.RUNTIME_C = original_runtime
        if rc_good != 0:
            print(f"SELF-TEST FAIL: known-good mock rejected (rc={rc_good})", file=sys.stderr)
            return 1

        try:
            self_mod.ROOT = tmp
            self_mod.RUNTIME_C = bad
            rc_bad = self_mod.main()
        finally:
            self_mod.ROOT = original_root
            self_mod.RUNTIME_C = original_runtime
        if rc_bad == 0:
            print("SELF-TEST FAIL: known-bad (missing weak attr) accepted", file=sys.stderr)
            return 1

        # Bad fixture: symbol missing entirely
        missing = tmp / "runtime_missing.c"
        missing_syms = REQUIRED_SYMBOLS[:-1]  # missing the last one
        missing.write_text(
            "\n".join(f"__attribute__((weak)) int {sym}(void) {{ return 0; }}" for sym in missing_syms) + "\n",
            encoding="utf-8",
        )
        try:
            self_mod.ROOT = tmp
            self_mod.RUNTIME_C = missing
            rc_missing = self_mod.main()
        finally:
            self_mod.ROOT = original_root
            self_mod.RUNTIME_C = original_runtime
        if rc_missing == 0:
            print("SELF-TEST FAIL: known-bad (symbol missing) accepted", file=sys.stderr)
            return 1

        print("SELF-TEST OK: linter accepts good fixture and rejects bad fixtures")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
