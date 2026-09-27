#!/usr/bin/env python3
"""Issue #4128 source-cite gate: WorkspaceUniqueIfNeeded lock_order stamp.

Soft --serve-async died with SIGABRT (std::system_error "Resource deadlock
avoided" = EDEADLK) on a define + call after a prior exec: the
(eval-current) primitive takes workspace_mtx_ via WorkspaceUniqueIfNeeded,
which acquired the non-recursive shared_mutex with a RAW unique_lock and
never stamped lock_order::on_acquire(Level::Workspace). The nested
aura_drop_jit_fn_native_for_define probe (lock_order::is_held(Workspace),
the #3441 already-held guard) therefore read false while this thread
already owned the mutex, took the re-lock arm
(aura_lock_workspace_write → lock_workspace_unique), and pthread_rwlock
returned EDEADLK on the same thread → terminate → SIGABRT (serve session
dies; client sees serve_session_timeout). Depth is correctness, not
observability — the same #2354 lesson.

ACs:
  AC1  WorkspaceUniqueIfNeeded ctor stamps on_acquire(Level::Workspace) on
       the owning branch BEFORE the unique_lock acquisition; cites #4128.
  AC2  the class dtor pairs the stamp with the release (mutex unlock then
       on_release(Level::Workspace)); cites #4128.
  AC3  move ctor + move assignment transfer the stamp
       (std::exchange(o.owns_unique_, false)) — a defaulted move would
       leave owns_unique_ true in the moved-from object and the dtor would
       drop a depth it no longer owns.
  AC4  the nested consumer keeps its already-held probe:
       aura_drop_jit_fn_native_for_define consults
       lock_order::is_held(Level::Workspace) and routes both arms through
       drop_jit_fn_native_for_define_locked (no direct mutation when held).
  AC5  tests cite #4128 (runtime ACs in test_fiber_concurrent_unit_batch);
       no tests/serve/test_issue_4128.cpp; no docs/design/4128-*;
       build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IXX = ROOT / "src" / "compiler" / "evaluator.ixx"
JIT = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
TST = ROOT / "tests" / "serve" / "test_fiber_concurrent_unit_batch.cpp"


def _class_body(text: str) -> str:
    """WorkspaceUniqueIfNeeded class region (first '};' after the header)."""
    idx = text.find("class WorkspaceUniqueIfNeeded")
    if idx < 0:
        return ""
    end = text.find("};", idx)
    return text[idx:end] if end > 0 else ""


def _drop_native_body(text: str) -> str:
    """aura_drop_jit_fn_native_for_define body (up to the next extern "C")."""
    idx = text.find('extern "C" void aura_drop_jit_fn_native_for_define')
    if idx < 0:
        return ""
    end = text.find('extern "C"', idx + 8)
    return text[idx:end] if end > 0 else text[idx : idx + 2000]


def main() -> int:
    ixx = IXX.read_text() if IXX.exists() else ""
    jit = JIT.read_text() if JIT.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    cls = _class_body(ixx)

    stamp = cls.find("aura::compiler::lock_order::on_acquire(")
    take = cls.find("std::unique_lock<std::shared_mutex>(ev.workspace_mtx_)")
    good = (
        "#4128" in cls
        and stamp >= 0
        and take >= 0
        and stamp < take
        and "aura::compiler::lock_order::Level::Workspace);" in cls
    )
    report(
        "AC1",
        good,
        "ctor stamps on_acquire(Level::Workspace) on the owning branch before the take; cites #4128",
    )

    dtor_idx = cls.find("~WorkspaceUniqueIfNeeded()")
    end_idx = cls.find("[[nodiscard]]", dtor_idx) if dtor_idx >= 0 else -1
    dtor_body = cls[dtor_idx:end_idx] if dtor_idx >= 0 and end_idx > dtor_idx else ""
    good = (
        "#4128" in cls
        and dtor_idx >= 0
        and "lock_.unlock();" in dtor_body
        and "aura::compiler::lock_order::on_release(" in dtor_body
    )
    report("AC2", good, "dtor pairs unlock with on_release(Level::Workspace); cites #4128")

    good = cls.count("std::exchange(o.owns_unique_, false)") == 2
    report("AC3", good, "move ctor + move assign transfer the stamp (no defaulted move)")

    drop = _drop_native_body(jit)
    good = (
        "lock_order::is_held(aura::compiler::lock_order::Level::Workspace)" in drop
        and "if (ws_held)" in drop
        and drop.count("drop_jit_fn_native_for_define_locked(name);") >= 2
    )
    report(
        "AC4",
        good,
        "nested drop-native keeps the is_held(Workspace) probe and the _locked variant in both arms",
    )

    no_doc = not any((ROOT / "docs" / "design").glob("4128-*")) if (ROOT / "docs" / "design").exists() else True
    good = (
        "#4128 AC1" in tst
        and "#4128 AC2" in tst
        and "#4128 AC3" in tst
        and "#4128 AC4" in tst
        and "#4128 AC5" in tst
        and "run_4128_serve_define_call_alive_smoke" in tst
        and not (ROOT / "tests" / "serve" / "test_issue_4128.cpp").exists()
        and no_doc
        and "check_workspace_lock_stamp_4128.py" in build
        and "check_workspace_lock_stamp_4128.py" in allow
    )
    report("AC5", good, "tests cite #4128; no issue-file/doc; build.py + allowlist wired")

    print(f"check_workspace_lock_stamp_4128: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
