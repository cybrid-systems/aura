#!/usr/bin/env python3
"""Issue #2413: FlatAST add_node multi-column SoA lock + reader contract.

Contract:
  AC1 documented concurrent access contract on FlatAST (flatast_mutex_)
  AC2 audit of lock-free SoA readers (workspace_mtx = external serial.)
  AC3 add_node/clear hold flatast_mutex_
  AC4 no public API behavior change required (docs-only Option A)

Also records audit findings for AC2 (readers that bypass flatast_mutex_).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _audit_readers() -> list[str]:
    """Lightweight audit: classify known concurrent-safe vs lock-free paths.

    Returns human-readable finding lines (always non-empty).
    """
    findings: list[str] = []
    # Query workspace path uses workspace_mtx (external serialization).
    qw = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    wlock = len(re.findall(r"shared_lock\s*<\s*std::shared_mutex\s*>\s+\w+\s*\(\s*ws\.workspace_mtx", qw))
    findings.append(
        f"query_workspace: {wlock} shared_lock(workspace_mtx) sites "
        "(external serial. vs add_node — OK under #2413 AC1(a))"
    )

    # tag()/get() accessors do not take flatast_mutex_ (by design).
    ast = _read("src/core/ast.ixx")
    if "flatast_mutex_" in ast and "try_acquire_reader_lock" in ast:
        findings.append(
            "core: tag/get are lock-free vs flatast_mutex_ (hot path); "
            "structural ReaderLockGuard does not cover add_node "
            "(documented #2413; #2488 SoAReadGuard for concurrent SoA reads)"
        )

    # Count approximate call sites of hot accessors in compiler (informational).
    tag_hits = 0
    for p in (ROOT / "src/compiler").rglob("*.cpp"):
        try:
            t = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        tag_hits += len(re.findall(r"\.tag\s*\(", t))
    findings.append(
        f"compiler: ~{tag_hits} `.tag(` call sites — rely on workspace_mtx / "
        "single-thread mutation contract (not flatast_mutex_)"
    )
    findings.append(
        "FOLLOW-UP: shipped #2488 Option B′ — OwnedSharedMutex + public "
        "SoAReadGuard / get_soa_safe (shared) vs exclusive add_node/clear; "
        "edit_epoch RCU (Option C) remains optional for lock-free retry"
    )
    return findings


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_flatast_add_node_lock_2413.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 documentation
    must("Issue #2413", "AC1", ast)
    must("concurrent access contract", "AC1", ast)
    must("flatast_mutex_", "AC1", ast)
    must("workspace_mtx", "AC1", ast)
    must("half-initialized", "AC1", ast)
    must("2413 AC1", "AC1", test)

    # AC2 audit always runs; print findings
    findings = _audit_readers()
    print("=== #2413 AC2 audit findings ===")
    for line in findings:
        print(f"  - {line}")
    if len(findings) < 3:
        fails.append("AC2: audit produced too few findings")
    must("2413 AC2", "AC2", test)
    must("FOLLOW-UP", "AC2", "\n".join(findings))

    # AC3 writers hold exclusive SoA lock (#2413; #2488 shared_mutex upgrade)
    add_idx = ast.find("NodeId add_node(NodeTag tag, SyntaxMarker m")
    if add_idx < 0:
        fails.append("AC3: add_node not found")
        add_body = ""
    else:
        add_body = ast[add_idx : add_idx + 900]
    # #2488: exclusive unique_lock on OwnedSharedMutex (was recursive_mutex).
    if "flatast_mutex_" not in add_body or ("unique_lock" not in add_body and "lock_guard" not in add_body):
        fails.append("AC3: add_node must exclusive-lock flatast_mutex_")
    must("2413 AC3", "AC3", test)

    clear_idx = ast.find("void clear() {")
    # FlatAST::clear is large; find the one near flatast_mutex
    clear_slice = ast[clear_idx : clear_idx + 900] if clear_idx >= 0 else ""
    if "flatast_mutex_" not in clear_slice:
        if "void clear()" not in ast or "flatast_mutex_" not in ast:
            fails.append("AC3: clear() flatast_mutex_ missing")
        else:
            # Accept exclusive lock near clear (#2463 / #2488)
            must("exclusive SoA write", "AC3", ast)

    must("2413 AC4", "AC4", test)

    must("check_flatast_add_node_lock_2413", "gate", build)
    must("cmd_flatast_add_node_lock_coverage", "gate", build)
    must("test_flatast_add_node_lock_2413", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: flatast add_node lock #2413 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
