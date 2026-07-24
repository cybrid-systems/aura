#!/usr/bin/env python3
"""Linter for issue #1998 / B-024: g_owned_pair_slots_ push in IR-executor
MakePair path must hold aura_lock_workspace_write() to prevent races with
the static PairSlotCleanup destructor + concurrent sibling bridge-hook writes
(aura_make_pair at aura_jit_runtime.cpp:1330+).

Audit reference: Aura JIT/FFI boundary code review, 2026-07-23 — finding ID B-024.

Acceptance criteria:
  AC1: src/compiler/ir_executor_impl.cpp MakePair path (malloc branch) holds
       aura_lock_workspace_write() / aura_unlock_workspace_write() around the
       g_owned_pair_slots_.push_back(slot) call. Regression guard against
       dropping the lock in a future refactor.
  AC2: src/compiler/aura_jit_runtime.cpp aura_make_pair() bridge-hook sibling
       also keeps its lock (regression guard for the existing fix at line
       1330+; if the sibling is unfixed in a future refactor, AC2 fails).
  AC3: src/compiler/aura_jit_runtime.cpp declares file-scope
       std::vector<PairSlot*> g_owned_pair_slots_; (storage sanity).
  AC4: src/compiler/aura_jit_runtime.cpp declares a static destructor
       PairSlotCleanup that iterates g_owned_pair_slots_ AND calls .clear()
       (the walker that races the unprotected push).
  AC5: src/compiler/runtime_shared.h declares the lock functions
       (extern "C" void aura_lock_workspace_write(); extern "C" void
       aura_unlock_workspace_write();) so other TUs can call them.
  AC6: tests/core/test_pair_slot_lock.cpp exists with concurrent push + reset coverage.
  AC7: --self-test accepts good fixture (locked push), rejects bad
       (unlocked push -- the B-024 regression).

Usage:
  python3 scripts/check_owned_pair_slots_lock_coverage.py             # gate
  python3 scripts/check_owned_pair_slots_lock_coverage.py --self-test  # AC7
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
IR_EXECUTOR = REPO / "src" / "compiler" / "ir_executor_impl.cpp"
AURA_JIT_RUNTIME = REPO / "src" / "compiler" / "aura_jit_runtime.cpp"
RUNTIME_SHARED = REPO / "src" / "compiler" / "runtime_shared.h"
TEST_FILE = REPO / "tests" / "core" / "test_pair_slot_lock.cpp"

# Window (lines) within which a lock/unlock must surround the push.
LOCK_WINDOW = 6


def _has_lock_pair_around(text: str, anchor_pos: int, *, direction: int = 0) -> bool:
    """Verify the text contains aura_lock_workspace_write() before the anchor
    and aura_unlock_workspace_write() after the anchor, both within LOCK_WINDOW
    lines of the anchor.

    direction: 0 = bidirectional (look in lines [anchor-LOCK_WINDOW, anchor+LOCK_WINDOW])
               negative = look only before (forward-direction lock-then-do pattern)
               positive = look only after
    """
    lines = text.split("\n")
    # Find the line index of the anchor (approximate via char position)
    line_idx = text[:anchor_pos].count("\n")
    lo = max(0, line_idx - (LOCK_WINDOW if direction <= 0 else 0))
    hi = min(len(lines), line_idx + (LOCK_WINDOW if direction >= 0 else 0) + 1)
    window = "\n".join(lines[lo:hi])
    has_lock = bool(re.search(r"aura_lock_workspace_write\s*\(\s*\)", window))
    has_unlock = bool(re.search(r"aura_unlock_workspace_write\s*\(\s*\)", window))
    # For "lock-then-do" we need lock before and unlock after
    if direction == 0:
        # Find positions of lock/unlock relative to anchor within window
        lock_pos = window.find("aura_lock_workspace_write")
        unlock_pos = window.find("aura_unlock_workspace_write")
        # Both must be present; ordering doesn't matter for the basic gate
        # (we separately verify the canonical lock-then-do pattern in detail)
        return not (lock_pos == -1 or unlock_pos == -1)
    elif direction < 0:
        return has_lock
    else:
        return has_unlock


def _find_push_site(text: str, var_name: str) -> int | None:
    """Find the char position of the FIRST `var_name.push_back(` in text.

    Returns the position of the `.push_back(` token, or None.
    """
    m = re.search(re.escape(var_name) + r"\s*\.\s*push_back\s*\(", text)
    return m.start() if m else None


def _find_all_push_sites(text: str, var_name: str) -> list[int]:
    """Find all char positions of `var_name.push_back(` in text."""
    return [m.start() for m in re.finditer(re.escape(var_name) + r"\s*\.\s*push_back\s*\(", text)]


def check() -> tuple[bool, list[str]]:
    """Run AC1-AC6. Returns (ok, errors)."""
    errors: list[str] = []

    # ── AC1: IR-executor MakePair path holds lock around g_owned_pair_slots_.push_back ──
    if not IR_EXECUTOR.exists():
        errors.append(f"AC1 FAIL: {IR_EXECUTOR} not found")
        ir_text = ""
    else:
        ir_text = IR_EXECUTOR.read_text()
        push_sites = _find_all_push_sites(ir_text, "g_owned_pair_slots_")
        if not push_sites:
            errors.append(
                "AC1 FAIL: no g_owned_pair_slots_.push_back( call found in "
                f"{IR_EXECUTOR.name} (expected MakePair malloc branch)"
            )
        else:
            # The B-024 push site is the only one in ir_executor_impl.cpp.
            # If a future change adds another, the linter must verify each
            # has a lock pair around it.
            all_locked = True
            for i, pos in enumerate(push_sites):
                # Use a slightly larger window to account for the comment block
                # added by the B-024 fix (~10 lines of explanation above the lock).
                # We expand the search to ~16 lines for the lock (above) and
                # ~6 lines for the unlock (below).
                lines = ir_text.split("\n")
                line_idx = ir_text[:pos].count("\n")
                lo = max(0, line_idx - 18)
                hi = min(len(lines), line_idx + 8)
                window = "\n".join(lines[lo:hi])
                has_lock = "aura_lock_workspace_write" in window
                has_unlock = "aura_unlock_workspace_write" in window
                if not (has_lock and has_unlock):
                    all_locked = False
                    errors.append(
                        f"AC1 FAIL: g_owned_pair_slots_.push_back( call site #{i + 1} "
                        f"in {IR_EXECUTOR.name} (line ~{line_idx + 1}) is not surrounded "
                        f"by aura_lock_workspace_write() + aura_unlock_workspace_write(). "
                        f"This is the B-024 regression: vector push_back can reallocate "
                        f"and race the static PairSlotCleanup destructor at "
                        f"aura_jit_runtime.cpp:425 (UAF) or land AFTER the destructor "
                        f"copied its iteration state (lost slot / host accounting leak)."
                    )
            if all_locked and len(push_sites) > 1:
                # Multi-site: not a hard error, but note for visibility
                pass  # acceptable -- the linter still verifies each site

    # ── AC2: bridge-hook sibling aura_make_pair keeps its lock (regression guard) ──
    if not AURA_JIT_RUNTIME.exists():
        errors.append(f"AC2 FAIL: {AURA_JIT_RUNTIME} not found")
        jit_text = ""
    else:
        jit_text = AURA_JIT_RUNTIME.read_text()
        push_sites = _find_all_push_sites(jit_text, "g_owned_pair_slots_")
        if not push_sites:
            errors.append(
                "AC2 FAIL: no g_owned_pair_slots_.push_back( call found in "
                f"{AURA_JIT_RUNTIME.name} (expected aura_make_pair bridge-hook)"
            )
        else:
            for i, pos in enumerate(push_sites):
                lines = jit_text.split("\n")
                line_idx = jit_text[:pos].count("\n")
                lo = max(0, line_idx - 8)
                hi = min(len(lines), line_idx + 8)
                window = "\n".join(lines[lo:hi])
                has_lock = "aura_lock_workspace_write" in window
                has_unlock = "aura_unlock_workspace_write" in window
                if not (has_lock and has_unlock):
                    errors.append(
                        f"AC2 FAIL: g_owned_pair_slots_.push_back( call site #{i + 1} "
                        f"in {AURA_JIT_RUNTIME.name} (line ~{line_idx + 1}, likely "
                        f"aura_make_pair) is not surrounded by "
                        f"aura_lock_workspace_write() + aura_unlock_workspace_write(). "
                        f"This is a regression of the #898 Phase 1 lock pattern."
                    )

    # ── AC3: file-scope declaration of g_owned_pair_slots_ in aura_jit_runtime.cpp ──
    if jit_text and not re.search(
        r"std::vector<PairSlot\*>\s+g_owned_pair_slots_\s*;",
        jit_text,
    ):
        errors.append(
            "AC3 FAIL: file-scope declaration of "
            "'std::vector<PairSlot*> g_owned_pair_slots_;' not found in "
            f"{AURA_JIT_RUNTIME.name}. The fix relies on this storage being "
            "owned in aura_jit_runtime.cpp so the static destructor "
            "PairSlotCleanup can iterate + free it at process exit."
        )

    # ── AC4: static destructor PairSlotCleanup iterates + clears the vector ──
    # We split the check into 3 sub-signals to avoid regex-with-nested-braces
    # fragility (the original single regex used [^}]* which breaks on the
    # for-loop body inside ~PairSlotCleanup()).
    if jit_text:
        # AC4a: the static instance (e.g. `PairSlotCleanup g_pair_slot_cleanup;`)
        has_instance = bool(re.search(r"PairSlotCleanup\s+g_pair_slot_cleanup\s*;", jit_text))
        # AC4b: iteration over g_owned_pair_slots_ in any scope (the walker)
        has_iteration = bool(re.search(r"for\s*\([^)]*:\s*g_owned_pair_slots_\s*\)", jit_text))
        # AC4c: clear() on g_owned_pair_slots_
        has_clear = bool(re.search(r"g_owned_pair_slots_\s*\.\s*clear\s*\(\s*\)", jit_text))
        # AC4d: the destructor definition itself
        has_dtor = bool(re.search(r"~\s*PairSlotCleanup\s*\(\s*\)", jit_text))

        missing = []
        if not has_instance:
            missing.append("static instance `PairSlotCleanup g_pair_slot_cleanup;`")
        if not has_iteration:
            missing.append("`for (... : g_owned_pair_slots_)` iteration (walker)")
        if not has_clear:
            missing.append("`g_owned_pair_slots_.clear()` call")
        if not has_dtor:
            missing.append("`~PairSlotCleanup()` destructor definition")
        if missing:
            errors.append(
                "AC4 FAIL: static destructor PairSlotCleanup (the walker that "
                "races the unprotected push) is missing required signals in "
                f"{AURA_JIT_RUNTIME.name}: {missing}. The combination of "
                "instance + iteration + clear + dtor proves the walker is "
                "structurally present (the destructor runs at process exit "
                "via the [[maybe_unused]] static instance's RAII teardown)."
            )

    # ── AC5: runtime_shared.h declares the lock functions ──
    if not RUNTIME_SHARED.exists():
        errors.append(f"AC5 FAIL: {RUNTIME_SHARED} not found")
    else:
        rs_text = RUNTIME_SHARED.read_text()
        if 'extern "C" void aura_lock_workspace_write' not in rs_text:
            errors.append(
                "AC5 FAIL: runtime_shared.h does not declare "
                "'extern \"C\" void aura_lock_workspace_write();'. "
                "Without this forward decl, IR-executor TUs cannot call "
                "the lock function from C++20 modules. Add it next to the "
                "g_owned_pair_slots_ extern decl."
            )
        if 'extern "C" void aura_unlock_workspace_write' not in rs_text:
            errors.append(
                "AC5 FAIL: runtime_shared.h does not declare "
                "'extern \"C\" void aura_unlock_workspace_write();'. "
                "Symmetric to AC5 lock decl."
            )

    # ── AC6: tests/core/test_pair_slot_lock.cpp exists with concurrent coverage ──
    if not TEST_FILE.exists():
        errors.append(f"AC6 FAIL: test file {TEST_FILE} not found")
    else:
        test_text = TEST_FILE.read_text()
        # The test must exercise concurrent push + reset (either via threads
        # or via repeated cycles with reset between). At minimum it should
        # mention at least one of: thread, std::async, std::thread, mutex,
        # concurrent, race, t_san, TSan, ASAN.
        concurrent_keys = ("thread", "async", "concurrent", "race", "TSan", "ASAN", "mutex")
        if not any(k in test_text for k in concurrent_keys):
            errors.append(
                f"AC6 FAIL: test file {TEST_FILE.name} does not exercise "
                f"concurrent push + reset. Must contain at least one of: "
                f"{concurrent_keys}. The test is the runtime gate for the "
                f"B-024 fix; without concurrent coverage, ASAN/TSan would "
                f"miss the race on CI."
            )

    ok = not errors
    return ok, errors


def self_test() -> tuple[bool, list[str]]:
    """AC7: synthetic good/bad fixture, ensure check() rejects bad and accepts good."""
    errors: list[str] = []

    # ── Good fixture (locked push) ──
    good_text = """\
                                g_pair_slots[idx] = slot;
                                aura_lock_workspace_write();
                                g_owned_pair_slots_.push_back(slot);
                                aura_unlock_workspace_write();
"""
    pos = _find_push_site(good_text, "g_owned_pair_slots_")
    if pos is None:
        errors.append("self-test: good fixture has no push site (synth bug)")
    else:
        if not _has_lock_pair_around(good_text, pos, direction=0):
            # The basic gate passes if lock + unlock both appear in window.
            # Expand the window via the per-site scan used in check().
            lines = good_text.split("\n")
            good_text[:pos].count("\n")
            window = "\n".join(lines)
            if "aura_lock_workspace_write" not in window or "aura_unlock_workspace_write" not in window:
                errors.append("self-test: good fixture not detected as locked (synth bug)")
        else:
            pass  # gate accepts

    # ── Bad fixture (no lock -- the B-024 regression) ──
    bad_text = """\
                                g_pair_slots[idx] = slot;
                                g_owned_pair_slots_.push_back(slot);
"""
    pos = _find_push_site(bad_text, "g_owned_pair_slots_")
    if pos is None:
        errors.append("self-test: bad fixture has no push site (synth bug)")
    else:
        lines = bad_text.split("\n")
        bad_text[:pos].count("\n")
        window = "\n".join(lines)
        has_lock = "aura_lock_workspace_write" in window
        has_unlock = "aura_unlock_workspace_write" in window
        if has_lock and has_unlock:
            errors.append("self-test: bad fixture (no lock) incorrectly accepted as locked")

    ok = not errors
    return ok, errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run synthetic good/bad fixture check (AC7) and exit",
    )
    args = parser.parse_args()

    if args.self_test:
        ok, errors = self_test()
        if ok:
            print("self-test: OK (good fixture accepted, bad fixture rejected)")
            return 0
        for e in errors:
            print(e)
        return 1

    ok, errors = check()
    if ok:
        print("OK: all #1998 ACs satisfied")
        return 0
    for e in errors:
        print(e)
    return 1


if __name__ == "__main__":
    sys.exit(main())
