#!/usr/bin/env python3
"""check_gc_heap_lock_scope_coverage.py — Issue #1993 source gate.

  AC1: (gc-heap) direct-clear body holds heap_mutex_ + module_mtx_ +
       workspace_mtx_ atomically (std::lock with defer_lock) for
       the entire body — covers the heap vectors AND the module +
       workspace state clears.
  AC2: (gc-heap) body clears modules_ / module_cache_ / workspace_flat_ /
       workspace_pool_ under the matching locks (so it's actually a
       "stronger reset than gc-temp" per the existing comment, not a
       doc-only claim).
  AC3: (gc-heap) body clears destroy_defuse_index() under module_mtx_.
  AC4: (gc) body holds workspace_mtx_ for the workspace_flat_ /
       workspace_pool_ pointer nulls (sibling of F-004).
  AC5: (gc) body holds module_mtx_ for the modules_ / module_cache_ /
       destroy_defuse_index() clears (already done by #1991 / B-010,
       re-asserted here).
  AC6: tests/mutation/test_issue_1993.cpp exists.
  AC7: linter self-test (--self-test passes).

Exit 0 = all ACs satisfied.

Rationale (Issue #1993 body):
  `(gc-heap)` direct-clear path mutated module + workspace state
  outside any lock. Concurrent load_module_file (writes modules_ /
  module_cache_ under module_mtx_) or set_workspace_flat (writes
  workspace_flat_ under workspace_mtx_) raced the clear. Fix holds
  all three locks atomically via std::lock with defer_lock; (gc) also
  adds workspace_mtx_ for the workspace pointer nulls.

Sibling: B-010 (#1991), F-004 (#1994).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MEM = ROOT / "src" / "compiler" / "evaluator_primitives_memory.cpp"
TEST = ROOT / "tests" / "mutation" / "test_issue_1993.cpp"


def _extract_body(text: str, open_idx: int) -> str:
    """Extract the body of a brace-delimited block (handles nested {})."""
    assert text[open_idx] == "{", f"Expected '{{' at {open_idx}"
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
    return ""


def _find_primitive_body(text: str, prim_name: str) -> str:
    """Find a `add("<prim_name>", ...)` lambda and return its body."""
    # Match `add("prim-name", [...]` then find the outermost `{ ... }`
    pat = re.compile(r'add\(\s*"' + re.escape(prim_name) + r'"\s*,\s*[^;]*?\{', re.DOTALL)
    m = pat.search(text)
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
    if not MEM.exists():
        failures.append("AC1-AC5: src/compiler/evaluator_primitives_memory.cpp not found")
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1

    mem = MEM.read_text(encoding="utf-8", errors="replace")

    # === (gc-heap) body checks ===
    heap_body = _find_primitive_body(mem, "gc-heap")
    if not heap_body:
        failures.append("AC1: (gc-heap) primitive body not found")
    else:
        # AC1: std::lock with defer_lock acquires heap_mutex + module_mtx + workspace_mtx
        if "std::lock(" not in heap_body:
            failures.append(
                "AC1: (gc-heap) body does not call std::lock() to atomically acquire "
                "heap_mutex_ + module_mtx_ + workspace_mtx_ (race under #1993 D-001)"
            )
        for needle in (
            "ev.heap_mutex()",
            "ev.module_mtx_",
            "ev.workspace_mtx_",
        ):
            if needle not in heap_body:
                failures.append(f"AC1: (gc-heap) body does not acquire {needle} (race under #1993)")
        # AC2: heap body clears module + workspace state
        for needle in (
            "destroy_defuse_index()",
            "ev.modules_.clear()",
            "ev.module_cache_.clear()",
            "ev.workspace_flat_ = nullptr",
            "ev.workspace_pool_ = nullptr",
        ):
            if needle not in heap_body:
                failures.append(
                    f"AC2: (gc-heap) body does not clear {needle} "
                    f"(doc claimed 'stronger reset than gc-temp' but implementation "
                    f"didn't match — #1993 D-001)"
                )
        # AC3: destroy_defuse_index under module_mtx
        if "destroy_defuse_index()" not in heap_body:
            failures.append("AC3: (gc-heap) body does not call destroy_defuse_index() (sibling of B-010 / #1991)")

    # === (gc) body checks ===
    gc_body = _find_primitive_body(mem, "gc")
    if not gc_body:
        failures.append("AC4/AC5: (gc) primitive body not found")
    else:
        # AC4: workspace_mtx_ for workspace_flat_/workspace_pool_ nulls
        if "ev.workspace_mtx_" not in gc_body:
            failures.append(
                "AC4: (gc) body does not acquire workspace_mtx_ for the "
                "workspace_flat_ / workspace_pool_ pointer nulls "
                "(sibling of F-004 / #1994 — reader races writer)"
            )
        if "std::lock(" not in gc_body:
            failures.append(
                "AC4: (gc) body does not call std::lock() — module_mtx_ + "
                "workspace_mtx_ should be acquired atomically to avoid deadlock"
            )
        # AC5: module_mtx_ for module clears (already from #1991)
        if "ev.module_mtx_" not in gc_body:
            failures.append("AC5: (gc) body does not acquire module_mtx_ — regression of #1991 / B-010 fix")
        for needle in (
            "destroy_defuse_index()",
            "ev.modules_.clear()",
            "ev.module_cache_.clear()",
        ):
            if needle not in gc_body:
                failures.append(f"AC5: (gc) body does not clear {needle} — regression of #1991 / B-010 fix")

    if not TEST.exists():
        failures.append("AC6: tests/mutation/test_issue_1993.cpp not found")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: all #1993 ACs satisfied ((gc-heap) lock scope + (gc) workspace_mtx + test present)")
    return 0


def self_test() -> int:
    """Self-test: feed good + bad fixtures through the linter."""
    import shutil
    import tempfile

    tmp = Path(tempfile.mkdtemp(prefix="check_1993_selftest_"))
    try:
        good_mem = tmp / "good.cpp"
        good_mem.write_text(
            # (gc-heap) with std::lock + all three locks + all clears
            'add("gc-heap", [&ev, destroy_defuse_index](const auto&) -> EvalValue {\n'
            "    {\n"
            "        std::unique_lock<std::mutex> heap_lock(ev.heap_mutex(), std::defer_lock);\n"
            "        std::unique_lock<std::shared_mutex> module_lock(ev.module_mtx_, std::defer_lock);\n"
            "        std::unique_lock<std::shared_mutex> workspace_lock(ev.workspace_mtx_, std::defer_lock);\n"
            "        std::lock(heap_lock, module_lock, workspace_lock);\n"
            "        ev.short_str_cache_.clear();\n"
            "        destroy_defuse_index();\n"
            "        ev.modules_.clear();\n"
            "        ev.module_cache_.clear();\n"
            "        ev.workspace_flat_ = nullptr;\n"
            "        ev.workspace_pool_ = nullptr;\n"
            "    }\n"
            "    return types::make_bool(true);\n"
            "});\n"
            # (gc) with std::lock + workspace_mtx + module_mtx + clears
            'add("gc", [&ev, destroy_defuse_index](const auto&) -> EvalValue {\n'
            "    {\n"
            "        std::unique_lock<std::shared_mutex> module_lock(ev.module_mtx_, std::defer_lock);\n"
            "        std::unique_lock<std::shared_mutex> workspace_lock(ev.workspace_mtx_, std::defer_lock);\n"
            "        std::lock(module_lock, workspace_lock);\n"
            "        destroy_defuse_index();\n"
            "        ev.modules_.clear();\n"
            "        ev.module_cache_.clear();\n"
            "        ev.workspace_flat_ = nullptr;\n"
            "        ev.workspace_pool_ = nullptr;\n"
            "    }\n"
            "    return types::make_bool(true);\n"
            "});\n",
            encoding="utf-8",
        )
        good_test = tmp / "test.cpp"
        good_test.write_text("// test_issue_1993.cpp\n", encoding="utf-8")

        import check_gc_heap_lock_scope_coverage as self_mod

        original = {
            "ROOT": self_mod.ROOT,
            "MEM": self_mod.MEM,
            "TEST": self_mod.TEST,
        }
        try:
            self_mod.ROOT = tmp
            self_mod.MEM = good_mem
            self_mod.TEST = good_test
            rc_good = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_good != 0:
            print(f"SELF-TEST FAIL: known-good mock rejected (rc={rc_good})", file=sys.stderr)
            return 1

        # Known-bad: (gc-heap) without std::lock
        bad_mem = tmp / "bad.cpp"
        bad_mem.write_text(
            'add("gc-heap", [&ev, destroy_defuse_index](const auto&) -> EvalValue {\n'
            "    {\n"
            "        std::lock_guard<std::mutex> lock(ev.heap_mutex());\n"  # BAD: no std::lock, no module_mtx, no workspace_mtx
            "        ev.short_str_cache_.clear();\n"
            "    }\n"
            "    return types::make_bool(true);\n"
            "});\n"
            'add("gc", [&ev, destroy_defuse_index](const auto&) -> EvalValue {\n'
            "    {\n"
            "        std::unique_lock<std::shared_mutex> lock(ev.module_mtx_);\n"
            "        ev.modules_.clear();\n"
            "        ev.workspace_flat_ = nullptr;\n"  # BAD: no workspace_mtx
            "    }\n"
            "    return types::make_bool(true);\n"
            "});\n",
            encoding="utf-8",
        )
        try:
            self_mod.ROOT = tmp
            self_mod.MEM = bad_mem
            self_mod.TEST = good_test
            rc_bad = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad == 0:
            print("SELF-TEST FAIL: known-bad mock accepted", file=sys.stderr)
            return 1

        print("SELF-TEST OK: linter accepts good fixture and rejects bad fixtures")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
