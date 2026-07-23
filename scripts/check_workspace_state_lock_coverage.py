#!/usr/bin/env python3
"""check_workspace_state_lock_coverage.py — Issue #1994 source gate.

  AC1: (workspace-state) body in evaluator_primitives_agent.cpp takes
       a shared_lock on workspace_mtx_ before reading workspace_flat_ /
       workspace_pool_ (the racy NULL-check-then-dereference pattern).
  AC2: (workspace:rollback-latest) in evaluator_primitives_workspace.cpp
       takes a unique_lock on workspace_mtx_ (reads + writes — set_child
       / mark_dirty_upward / status update).
  AC3: (workspace:mutation-count) in evaluator_primitives_workspace.cpp
       takes a shared_lock on workspace_mtx_ (read-only).
  AC4: (workspace:create) in evaluator_primitives_workspace.cpp
       takes a unique_lock on workspace_mtx_ (writer — updates
       workspace_tree_ and the root node's flat/pool).
  AC5: (workspace:resolve-stable-ref) in evaluator_primitives_workspace.cpp
       takes a shared_lock on workspace_mtx_ (read-only).
  AC6: tests/mutation/test_issue_1994.cpp exists.
  AC7: linter self-test (--self-test passes).

Exit 0 = all ACs satisfied.

Rationale (Issue #1994 body):
  `(workspace-state)` and `(workspace-tree-*)` primitives had a
  NULL check followed 5 lines later by dereference of
  `*ev.workspace_flat_` / `*ev.workspace_pool_`. Concurrent
  `set_workspace_flat` (which holds unique_lock(workspace_mtx_) but
  may roll back on build_tag_arity_index throw — see evaluator.ixx:2683)
  could free/swap the FlatAST* between the check and the
  dereference → use-after-free / wrong-data / traversal into freed
  memory.

  Fix: take shared_lock (or unique_lock for read+write sites) on
  workspace_mtx_ for the body of every primitive that touches
  workspace_flat_ / workspace_pool_. WorkspaceFlatPin
  (evaluator.ixx:1552) is the RAII helper but only pins the
  FlatAST* (not the pool), so explicit shared_lock + dereference
  is the pattern used for these primitives.

Sibling: B-010 (#1991), D-001 (#1993), this issue.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AGENT = ROOT / "src" / "compiler" / "evaluator_primitives_agent.cpp"
WORK = ROOT / "src" / "compiler" / "evaluator_primitives_workspace.cpp"
TEST = ROOT / "tests" / "mutation" / "test_issue_1994.cpp"


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
    """Find a `add("<prim_name>", ...)` or `register_stats_impl("<prim_name>", ...)`
    lambda and return its body."""
    pat_add = re.compile(r'add\(\s*"' + re.escape(prim_name) + r'"\s*,\s*[^;]*?\{', re.DOTALL)
    pat_reg = re.compile(
        r'register_stats_impl\(\s*"' + re.escape(prim_name) + r'"\s*,\s*[^;]*?\{',
        re.DOTALL,
    )
    m = pat_add.search(text) or pat_reg.search(text)
    if not m:
        return ""
    open_idx = text.find("{", m.end() - 1)
    if open_idx < 0:
        return ""
    return _extract_body(text, open_idx)


def _check_lock(body: str, prim_name: str, lock_type: str, failures: list[str]) -> None:
    """Verify the body contains the requested workspace_mtx_ lock pattern."""
    if lock_type == "shared":
        ok = "std::shared_lock<std::shared_mutex>" in body and "workspace_mtx_" in body
    elif lock_type == "unique":
        ok = "std::unique_lock<std::shared_mutex>" in body and "workspace_mtx_" in body
    else:
        raise ValueError(f"Unknown lock_type {lock_type!r}")
    if not ok:
        needle = (
            "std::shared_lock<std::shared_mutex>" if lock_type == "shared" else "std::unique_lock<std::shared_mutex>"
        )
        failures.append(
            f"AC: {prim_name} body does not take {needle} on workspace_mtx_ — "
            f"F-004 racy NULL-check-then-dereference under #1994"
        )


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    failures: list[str] = []

    # === AC1: (workspace-state) in agent.cpp ===
    if not AGENT.exists():
        failures.append("AC1: src/compiler/evaluator_primitives_agent.cpp not found")
    else:
        agent = AGENT.read_text(encoding="utf-8", errors="replace")
        body = _find_primitive_body(agent, "workspace-state")
        if not body:
            failures.append("AC1: (workspace-state) primitive body not found")
        else:
            _check_lock(body, "workspace-state", "shared", failures)

    # === AC2-AC5: (workspace-tree-*) in workspace.cpp ===
    if not WORK.exists():
        failures.append("AC2-AC5: src/compiler/evaluator_primitives_workspace.cpp not found")
    else:
        work = WORK.read_text(encoding="utf-8", errors="replace")
        specs = [
            ("workspace:rollback-latest", "unique"),
            ("workspace:mutation-count", "shared"),
            ("workspace:create", "unique"),
            ("workspace:resolve-stable-ref", "shared"),
        ]
        for prim, lock in specs:
            body = _find_primitive_body(work, prim)
            if not body:
                failures.append(f"AC: {prim} primitive body not found")
            else:
                _check_lock(body, prim, lock, failures)

    # === AC6: test file exists ===
    if not TEST.exists():
        failures.append("AC6: tests/mutation/test_issue_1994.cpp not found")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print(
        "OK: all #1994 ACs satisfied (workspace-state + workspace:rollback-latest + workspace:mutation-count + workspace:create + workspace:resolve-stable-ref all take workspace_mtx_ lock)"
    )
    return 0


def self_test() -> int:
    """Self-test: feed good + bad fixtures through the linter."""
    import shutil
    import tempfile

    tmp = Path(tempfile.mkdtemp(prefix="check_1994_selftest_"))
    try:
        # Build a single C++ file containing all 5 primitives — good + bad variants.
        # Each primitive in its OWN good file (the linter reads per-file)
        good_agent = tmp / "agent.cpp"
        good_agent.write_text(
            'add("workspace-state", [&ev](const auto&) -> EvalValue {\n'
            "    std::shared_lock<std::shared_mutex> lock(ev.workspace_mtx_);\n"
            "    if (!ev.workspace_flat_) return make_int(0);\n"
            "    auto& flat = *ev.workspace_flat_;\n"
            "    return make_int(0);\n"
            "});\n",
            encoding="utf-8",
        )
        good_work = tmp / "workspace.cpp"
        good_work.write_text(
            'add("workspace:rollback-latest", [&ev](const auto&) -> EvalValue {\n'
            "    std::unique_lock<std::shared_mutex> lock(ev.workspace_mtx_);\n"
            "    return make_int(0);\n"
            "});\n"
            'register_stats_impl("workspace:mutation-count", [&ev](const auto&) -> EvalValue {\n'
            "    std::shared_lock<std::shared_mutex> lock(ev.workspace_mtx_);\n"
            "    return make_int(0);\n"
            "});\n"
            'add("workspace:create", [&ev](std::span<const EvalValue> a) -> EvalValue {\n'
            "    std::unique_lock<std::shared_mutex> lock(ev.workspace_mtx_);\n"
            "    return make_int(0);\n"
            "});\n"
            'add("workspace:resolve-stable-ref", [&ev](std::span<const EvalValue> a) -> EvalValue {\n'
            "    std::shared_lock<std::shared_mutex> lock(ev.workspace_mtx_);\n"
            "    return make_int(0);\n"
            "});\n",
            encoding="utf-8",
        )
        good_test = tmp / "test.cpp"
        good_test.write_text("// test_issue_1994.cpp\n", encoding="utf-8")

        import check_workspace_state_lock_coverage as self_mod

        original = {
            "AGENT": self_mod.AGENT,
            "WORK": self_mod.WORK,
            "TEST": self_mod.TEST,
        }
        try:
            self_mod.AGENT = good_agent
            self_mod.WORK = good_work
            self_mod.TEST = good_test
            rc_good = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_good != 0:
            print(f"SELF-TEST FAIL: known-good mock rejected (rc={rc_good})", file=sys.stderr)
            return 1

        # Known-bad: workspace-state without shared_lock
        bad_agent = tmp / "agent_bad.cpp"
        bad_agent.write_text(
            'add("workspace-state", [&ev](const auto&) -> EvalValue {\n'
            "    if (!ev.workspace_flat_) return make_int(0);\n"  # BAD: no shared_lock
            "    auto& flat = *ev.workspace_flat_;\n"
            "    return make_int(0);\n"
            "});\n",
            encoding="utf-8",
        )
        try:
            self_mod.AGENT = bad_agent
            self_mod.WORK = good_work
            self_mod.TEST = good_test
            rc_bad = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad == 0:
            print("SELF-TEST FAIL: known-bad (workspace-state no lock) accepted", file=sys.stderr)
            return 1

        # Known-bad: workspace:mutation-count with unique_lock (should be shared_lock)
        bad_work = tmp / "work_bad.cpp"
        bad_work.write_text(
            'register_stats_impl("workspace:mutation-count", [&ev](const auto&) -> EvalValue {\n'
            "    std::unique_lock<std::shared_mutex> lock(ev.workspace_mtx_);\n"  # BAD: should be shared_lock
            "    return make_int(0);\n"
            "});\n",
            encoding="utf-8",
        )
        try:
            self_mod.AGENT = good_agent
            self_mod.WORK = bad_work
            self_mod.TEST = good_test
            rc_bad2 = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad2 == 0:
            print(
                "SELF-TEST FAIL: known-bad (workspace:mutation-count unique instead of shared) accepted",
                file=sys.stderr,
            )
            return 1

        print("SELF-TEST OK: linter accepts good fixture and rejects bad fixtures")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
