#!/usr/bin/env python3
"""Issue #2412: reset_node_slot always clears incoming_parent_edges_.

Contract:
  AC1 always clear on reset (not gated on !dirty)
  AC2 collect/rebuild still works (no dirty-flag ownership change)
  AC3 2nd-recycle test present
  AC4 raw edge count helper + clean-path coverage

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
            fails.append(f"{label}: unexpected {n!r}")

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_reset_slot_parent_edges.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Locate reset_node_slot body
    idx = ast.find("void reset_node_slot(NodeId id, NodeTag tag, SyntaxMarker m)")
    if idx < 0:
        fails.append("AC1: reset_node_slot not found")
        body = ""
    else:
        # reset_node_slot is long (many column resets + #2440 atomic_ref stores);
        # need ~3.5k for the edge clear at the tail of the body.
        body = ast[idx : idx + 3600]

    must("Issue #2412", "AC1", ast)
    must("incoming_parent_edges_[id].clear()", "AC1", body)
    # Old gated form must not remain
    must_not(
        "if (!incoming_parent_index_dirty_ && id < incoming_parent_edges_.size())",
        "AC1",
        body,
    )
    must("2412 AC1", "AC1", test)

    must("rebuild_incoming_parent_index", "AC2", ast)
    must("2412 AC2", "AC2", test)

    must("2412 AC3", "AC3", test)
    must("recycle_dead_nodes", "AC3", test)

    must("incoming_parent_edge_count_raw", "AC4", ast)
    must("2412 AC4", "AC4", test)

    must("check_reset_slot_parent_edges_2412", "gate", build)
    must("cmd_reset_slot_parent_edges_coverage", "gate", build)
    must("test_reset_slot_parent_edges", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: reset slot parent edges #2412 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
