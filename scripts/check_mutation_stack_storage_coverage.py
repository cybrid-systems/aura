#!/usr/bin/env python3
"""check_mutation_stack_storage_coverage.py — Issue #1992 source gate.

  AC1: Fiber::mutation_stack_storage_ is std::atomic<void*>, not plain void*
  AC2: Fiber::yield_checkpoint_storage_ is std::atomic<void*>, not plain void*
  AC3: Fiber exposes compare_exchange_mutation_stack_ptr (CAS for init)
  AC4: Fiber exposes compare_exchange_yield_checkpoint_ptr (CAS for init)
  AC5: ensure_mutation_stack_ptr body uses compare_exchange_weak
        (CAS-init pattern, not plain read-then-write)
  AC6: ensure_yield_stack_ptr body uses compare_exchange_weak
        (CAS-init pattern, not plain read-then-write)
  AC7: ensure_*_ptr release-on-CAS-failure path (loser releases
        allocation back to pool — no leak under contention)
  AC8: tests/mutation/test_issue_1992.cpp exists
  AC9: linter self-test (--self-test passes)

Exit 0 = all ACs satisfied.

Rationale (Issue #1992 body):
  Fiber::mutation_stack_storage_ / yield_checkpoint_storage_ were
  plain `void*` fields. Under work-stealing handoff, two workers
  concurrently entering ensure_mutation_stack_ptr for the same
  Fiber pointer could both see p==nullptr, both allocate, both
  write — last-writer wins, one pointer leaks (memory leak) and
  the fiber could resume on a stack another fiber still holds
  (use-after-free across fibers).

  Fix: std::atomic<void*> + compare_exchange_weak in
  ensure_mutation_stack_ptr / ensure_yield_stack_ptr. The CAS
  winner publishes; losers release their allocation back to the
  pool.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIBER_H = ROOT / "src" / "serve" / "fiber.h"
EVAL = ROOT / "src" / "compiler" / "evaluator_fiber_mutation.cpp"
TEST = ROOT / "tests" / "mutation" / "test_issue_1992.cpp"


def _extract_body(text: str, open_idx: int) -> str:
    """Extract the body of a brace-delimited block starting at open_idx.

    Uses a balanced-brace scan to handle nested {} correctly (the
    ensure_*_ptr functions have nested if/else blocks).
    """
    assert text[open_idx] == "{", f"Expected '{{' at {open_idx}, got {text[open_idx]!r}"
    depth = 0
    i = open_idx
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1 : i]
        i += 1
    return ""  # unbalanced


def _find_function_body(text: str, sig_regex: str) -> str:
    """Find a function signature and return its body."""
    m = re.search(sig_regex, text)
    if not m:
        return ""
    open_idx = text.find("{", m.end() - 1)
    if open_idx < 0:
        return ""
    return _extract_body(text, open_idx)


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    failures: list[str] = []
    if not FIBER_H.exists():
        failures.append("AC1/AC2/AC3/AC4: src/serve/fiber.h not found")
    else:
        fiber_h = FIBER_H.read_text(encoding="utf-8", errors="replace")
        if not re.search(r"std::atomic<void\*>\s+mutation_stack_storage_\s*\{?\s*nullptr", fiber_h):
            failures.append(
                "AC1: src/serve/fiber.h does not declare "
                "`std::atomic<void*> mutation_stack_storage_` "
                "(plain void* is the data race under #1992)"
            )
        if not re.search(r"std::atomic<void\*>\s+yield_checkpoint_storage_\s*\{?\s*nullptr", fiber_h):
            failures.append(
                "AC2: src/serve/fiber.h does not declare "
                "`std::atomic<void*> yield_checkpoint_storage_` "
                "(plain void* is the data race under #1992)"
            )
        if "compare_exchange_mutation_stack_ptr" not in fiber_h:
            failures.append(
                "AC3: src/serve/fiber.h does not expose "
                "`compare_exchange_mutation_stack_ptr` (CAS for "
                "concurrent ensure_mutation_stack_ptr init)"
            )
        if "compare_exchange_yield_checkpoint_ptr" not in fiber_h:
            failures.append(
                "AC4: src/serve/fiber.h does not expose "
                "`compare_exchange_yield_checkpoint_ptr` (CAS for "
                "concurrent ensure_yield_stack_ptr init)"
            )

    if not EVAL.exists():
        failures.append("AC5/AC6/AC7: src/compiler/evaluator_fiber_mutation.cpp not found")
    else:
        eval_cpp = EVAL.read_text(encoding="utf-8", errors="replace")

        # AC5: ensure_mutation_stack_ptr body uses compare_exchange_weak
        m_body = _find_function_body(
            eval_cpp,
            r"MutationStackVec\*\s+ensure_mutation_stack_ptr\(\s*"
            r"aura::serve::Fiber\*\s+fiber\s*\)",
        )
        if not m_body:
            failures.append("AC5: ensure_mutation_stack_ptr body not found (signature changed?)")
        else:
            if "compare_exchange_weak" not in m_body and "compare_exchange_mutation_stack_ptr" not in m_body:
                failures.append(
                    "AC5: ensure_mutation_stack_ptr body does not use "
                    "compare_exchange_weak / compare_exchange_mutation_stack_ptr "
                    "(race under #1992 — plain read-then-write)"
                )
            if "release_mutation_stack" not in m_body:
                failures.append(
                    "AC7: ensure_mutation_stack_ptr body does not call "
                    "release_mutation_stack on the CAS-failure path "
                    "(loser leaks allocation under contention)"
                )

        # AC6: ensure_yield_stack_ptr body uses compare_exchange_weak
        y_body = _find_function_body(
            eval_cpp,
            r"YieldStackVec\*\s+ensure_yield_stack_ptr\(\s*"
            r"aura::serve::Fiber\*\s+fiber\s*\)",
        )
        if not y_body:
            failures.append("AC6: ensure_yield_stack_ptr body not found (signature changed?)")
        else:
            if "compare_exchange_weak" not in y_body and "compare_exchange_yield_checkpoint_ptr" not in y_body:
                failures.append(
                    "AC6: ensure_yield_stack_ptr body does not use "
                    "compare_exchange_weak / compare_exchange_yield_checkpoint_ptr "
                    "(race under #1992 — plain read-then-write)"
                )
            if "release_yield_stack" not in y_body:
                failures.append(
                    "AC7: ensure_yield_stack_ptr body does not call "
                    "release_yield_stack on the CAS-failure path "
                    "(loser leaks allocation under contention)"
                )

    if not TEST.exists():
        failures.append("AC8: tests/mutation/test_issue_1992.cpp not found")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: all #1992 ACs satisfied (mutation_stack_storage atomic + CAS + test present)")
    return 0


def self_test() -> int:
    """Self-test the linter: feed it a known-good fixture and a known-bad fixture."""
    import shutil
    import tempfile

    tmp = Path(tempfile.mkdtemp(prefix="check_1992_selftest_"))
    try:
        # Build mock fixtures
        good_fiber_h = tmp / "fiber.h"
        good_fiber_h.write_text(
            "std::atomic<void*> mutation_stack_storage_{nullptr};\n"
            "std::atomic<void*> yield_checkpoint_storage_{nullptr};\n"
            "bool compare_exchange_mutation_stack_ptr(void*& e, void* d) noexcept;\n"
            "bool compare_exchange_yield_checkpoint_ptr(void*& e, void* d) noexcept;\n",
            encoding="utf-8",
        )
        good_eval = tmp / "evaluator_fiber_mutation.cpp"
        good_eval.write_text(
            "MutationStackVec* ensure_mutation_stack_ptr(aura::serve::Fiber* fiber) {\n"
            "    void* p = fiber->mutation_stack_ptr();\n"
            "    if (p == nullptr) {\n"
            "        MutationStackVec* fresh = acquire_mutation_stack();\n"
            "        void* expected = nullptr;\n"
            "        if (fiber->compare_exchange_mutation_stack_ptr(expected, fresh)) {\n"
            "            p = fresh;\n"
            "        } else {\n"
            "            release_mutation_stack(fresh);\n"
            "            p = fiber->mutation_stack_ptr();\n"
            "        }\n"
            "    }\n"
            "    return static_cast<MutationStackVec*>(p);\n"
            "}\n"
            "YieldStackVec* ensure_yield_stack_ptr(aura::serve::Fiber* fiber) {\n"
            "    void* p = fiber->yield_checkpoint_ptr();\n"
            "    if (p == nullptr) {\n"
            "        YieldStackVec* fresh = acquire_yield_stack();\n"
            "        void* expected = nullptr;\n"
            "        if (fiber->compare_exchange_yield_checkpoint_ptr(expected, fresh)) {\n"
            "            p = fresh;\n"
            "        } else {\n"
            "            release_yield_stack(fresh);\n"
            "            p = fiber->yield_checkpoint_ptr();\n"
            "        }\n"
            "    }\n"
            "    return static_cast<YieldStackVec*>(p);\n"
            "}\n",
            encoding="utf-8",
        )
        good_test = tmp / "test.cpp"
        good_test.write_text("// test_issue_1992.cpp\n", encoding="utf-8")

        # Patch ROOT/FIBER_H/EVAL/TEST, reload main, capture rc
        # Use importlib to reload with new module-level constants
        import check_mutation_stack_storage_coverage as self_mod

        original_globals = {
            "ROOT": self_mod.ROOT,
            "FIBER_H": self_mod.FIBER_H,
            "EVAL": self_mod.EVAL,
            "TEST": self_mod.TEST,
        }
        try:
            self_mod.ROOT = tmp
            self_mod.FIBER_H = good_fiber_h
            self_mod.EVAL = good_eval
            self_mod.TEST = good_test
            rc_good = self_mod.main()
        finally:
            for k, v in original_globals.items():
                setattr(self_mod, k, v)

        if rc_good != 0:
            print(f"SELF-TEST FAIL: known-good mock rejected (rc={rc_good})", file=sys.stderr)
            return 1

        # Flip one fixture to known-bad (plain void*) and confirm rejection
        bad_fiber_h = tmp / "fiber_bad.h"
        bad_fiber_h.write_text(
            "void* mutation_stack_storage_ = nullptr;\n"
            "void* yield_checkpoint_storage_ = nullptr;\n"
            "bool compare_exchange_mutation_stack_ptr(void*& e, void* d) noexcept;\n"
            "bool compare_exchange_yield_checkpoint_ptr(void*& e, void* d) noexcept;\n",
            encoding="utf-8",
        )
        try:
            self_mod.ROOT = tmp
            self_mod.FIBER_H = bad_fiber_h
            self_mod.EVAL = good_eval
            self_mod.TEST = good_test
            rc_bad = self_mod.main()
        finally:
            for k, v in original_globals.items():
                setattr(self_mod, k, v)

        if rc_bad == 0:
            print("SELF-TEST FAIL: known-bad mock accepted", file=sys.stderr)
            return 1

        # Also test: missing release_mutation_stack should fail AC7
        no_release_eval = tmp / "evaluator_no_release.cpp"
        no_release_eval.write_text(
            "MutationStackVec* ensure_mutation_stack_ptr(aura::serve::Fiber* fiber) {\n"
            "    void* p = fiber->mutation_stack_ptr();\n"
            "    if (p == nullptr) {\n"
            "        MutationStackVec* fresh = acquire_mutation_stack();\n"
            "        void* expected = nullptr;\n"
            "        if (fiber->compare_exchange_mutation_stack_ptr(expected, fresh)) {\n"
            "            p = fresh;\n"
            "        } else {\n"
            "            // BAD: should release here\n"
            "            p = fiber->mutation_stack_ptr();\n"
            "        }\n"
            "    }\n"
            "    return static_cast<MutationStackVec*>(p);\n"
            "}\n"
            "YieldStackVec* ensure_yield_stack_ptr(aura::serve::Fiber* fiber) {\n"
            "    void* p = fiber->yield_checkpoint_ptr();\n"
            "    if (p == nullptr) {\n"
            "        YieldStackVec* fresh = acquire_yield_stack();\n"
            "        void* expected = nullptr;\n"
            "        if (fiber->compare_exchange_yield_checkpoint_ptr(expected, fresh)) {\n"
            "            p = fresh;\n"
            "        } else {\n"
            "            // BAD: should release here\n"
            "            p = fiber->yield_checkpoint_ptr();\n"
            "        }\n"
            "    }\n"
            "    return static_cast<YieldStackVec*>(p);\n"
            "}\n",
            encoding="utf-8",
        )
        try:
            self_mod.ROOT = tmp
            self_mod.FIBER_H = good_fiber_h
            self_mod.EVAL = no_release_eval
            self_mod.TEST = good_test
            rc_no_rel = self_mod.main()
        finally:
            for k, v in original_globals.items():
                setattr(self_mod, k, v)

        if rc_no_rel == 0:
            print("SELF-TEST FAIL: known-bad (no release on CAS-failure) accepted", file=sys.stderr)
            return 1

        print("SELF-TEST OK: linter accepts good fixture and rejects bad fixtures")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
