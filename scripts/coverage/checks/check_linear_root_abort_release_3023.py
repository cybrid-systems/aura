#!/usr/bin/env python3
"""Issue #3023: leftover linear_roots unpin on abort / mutate-fail / reclaim.

Contract (one row per AC):
  AC1  abort / mutate-fail / fiber reclaim drain leftover linear_roots
       (live_count==0) via unpin_all_linear_roots. Nested outer roots
       stay until outermost fail or reclaim.
  AC2  Responsibility is a single audit face: post-join =
       Fiber::release_orphan_roots; post-abort =
       enforce_linear_post_failure; post-densify verify NEVER unpins.
  AC3  Tests: abort restore canary + live_count==0 + fiber reclaim soak
       in test_linear_pin_moving_compact. No test_issue_3023.cpp.
       No docs/design/ (#1655). Soft empty drain is one lock + empty
       check. No second pin/GC model.

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

    lp = _read("src/core/lifetime_pin.hh")
    ixx = _read("src/core/lifetime_pin.ixx")
    gc = _read("src/compiler/evaluator_gc.cpp")
    fib = _read("src/serve/fiber.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_linear_pin_moving_compact.cpp")
    build = _read("build.py")

    # ── AC1: drain helper + wiring ──
    must("unpin_all_linear_roots", "AC1 helper", lp)
    must("kLinearRootAbortReleaseIssue", "AC1 stamp", lp)
    must("g_linear_root_abort_release_total", "AC1 counter", lp)
    must("unpin_all_linear_roots", "AC1 post-failure", gc)
    must("unpin_all_linear_roots", "AC1 post-join", fib)
    must("Issue #3023", "AC1 gc cite", gc)
    must("Issue #3023", "AC1 fiber cite", fib)
    if "AgentRegistry" in lp[lp.find("Issue #3023") : lp.find("Issue #3023") + 2000]:
        fails.append("AC1: must not introduce AgentRegistry")

    # ── AC2: responsibility face ──
    must("post-join", "AC2 post-join", lp)
    must("post-abort", "AC2 post-abort", lp)
    must("post-densify", "AC2 post-densify", lp)
    must("this verify never unpins", "AC2 verify never unpins", lp)
    must("unpin_all_linear_roots", "AC2 export", ixx)

    # ── AC3: tests + Soft + no invent ──
    must("AC3023: abort restore canary", "AC3 canary", test)
    must("AC3023: live_count==0 after abort drain", "AC3 live_count", test)
    must("AC3023 soak: live_count==0", "AC3 soak", test)
    must("schema-3023", "AC3 query", obs)
    must("one lock + empty check", "AC3 Soft", lp)
    must("check_linear_root_abort_release_3023", "AC3 build", build)
    must("cmd_linear_root_abort_release_3023", "AC3 build cmd", build)
    for rel in (
        "tests/compiler/test_issue_3023.cpp",
        "tests/core/test_issue_3023.cpp",
    ):
        if _read(rel):
            fails.append(f"AC3: {rel} exists — forbidden per #81967")
    if _read("docs/design/3023-linear-root-abort-release.md"):
        fails.append("AC3: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3023 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3023 linear_roots abort/reclaim unpin — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
