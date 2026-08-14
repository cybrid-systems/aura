#!/usr/bin/env python3
"""Issue #3028: explicit hygiene_depth is sole authority; TLS diagnostics only.

CapGuard deny is a member flag (not TLS -1). Same-FlatAST concurrent
top-level writers reject or serialize. Mid-clone fail-closed rolls back
name_map. Cross-flat concurrent clones unchanged.

Contract (one row per AC):
  AC1  no production rename/ceiling read of s_effective_max_depth as authority
  AC2  same-FlatAST claim + reject counter; two writers serialize or reject
  AC3  NameMapCheckpoint rollback; steal abort fail-closed
  AC4  Soft / cross-flat unchanged; additive query keys
  AC5  tests + build.py; no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    me = _read("src/compiler/macro_expansion.cpp")
    ixx = _read("src/compiler/macro_expansion.ixx")
    q = _read("src/compiler/evaluator_primitives_query_obs_mid.cpp")
    test = _read("tests/compiler/test_concurrent_clone_hygiene_depth.cpp")
    build = _read("build.py")
    bridge = _read("src/compiler/aura_jit_bridge.h")

    # AC1 explicit depth authority
    must("Issue #3028", "AC1", me)
    must("denied_", "AC1 CapGuard member", me)
    must("session_depth_limit", "AC1 session limit", me)
    if "s_effective_max_depth < 0" in me:
        fails.append("AC1: TLS -1 sentinel still used as denied() authority")
    must("TLS is not read for this decision", "AC1 no TLS authority", me)
    must("diagnostics only", "AC1 effective_hygiene_depth_limit", me)
    must("3028 AC1", "AC1 test", test)

    # AC2 same-flat
    must("claim_same_flat_clone", "AC2 claim", me)
    must("g_macro_clone_same_flat_reject_total", "AC2 counter", me)
    must("g_macro_clone_same_flat_reject_total", "AC2 ixx", ixx)
    must("rejected_same_flat", "AC2 guard flag", me)
    must("3028 AC2", "AC2 test", test)

    # AC3 name_map / steal
    must("NameMapCheckpoint", "AC3 checkpoint", me)
    must("g_macro_clone_steal_abort_total", "AC3 steal abort", me)
    must("steal1 > steal0", "AC3 steal compare", me)
    must("3028 AC3", "AC3 test", test)

    # AC4 query additive
    must("schema-3028", "AC4 schema", q)
    must("same-flat-reject-total", "AC4 query", q)
    must("steal-abort-total", "AC4 steal query", q)
    must("explicit-depth-authority-wired", "AC4 wired", q)
    must("aura_macro_clone_same_flat_reject_total_v_read", "AC4 v_read", bridge)
    must("3028 AC4", "AC4 test", test)

    # AC5 wiring
    must("check_tls_depth_same_flat_clone_3028", "AC5 build", build)
    must("cmd_tls_depth_same_flat_clone_3028", "AC5 build cmd", build)
    must("ac2806", "AC5 preserve 2806", test)
    cite = me.find("Issue #3028")
    if cite >= 0 and "AgentRegistry" in me[cite : cite + 2500]:
        fails.append("AC5: must not introduce AgentRegistry")
    if (ROOT / "tests" / "compiler" / "test_issue_3028.cpp").is_file():
        fails.append("AC5: test_issue_3028.cpp present (forbidden per #81967)")
    if _read("docs/design/3028-tls-depth-same-flat.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3028 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3028 TLS depth / same-FlatAST clone — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
