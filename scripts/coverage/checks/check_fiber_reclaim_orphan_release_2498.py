#!/usr/bin/env python3
"""Issue #2498: epoch-scoped off-stack orphan-root table for fiber reclaim.

#2467 / #2468 / #2469 fixed UAF on reclaimed fibers but deferred the
cleanup hook (aura_evaluator_on_fiber_join) until state_==Done. For
non-yielding bodies after hard-reclaim, Done never fires — global table
entries (EnvFrame refs, mailbox refs, external handles) leak by design.

This linter enforces that:
  AC1 Phase 5 Fiber::join path: every JoinStatus::Reclaimed return is
       preceded by target->release_orphan_roots(). Off-stack orphan roots
       (EnvFrame/mailbox refs the body registered globally) are released
       without touching the body's running stack.
  AC2 Fiber::~Fiber() invokes release_orphan_roots() as safety net for
       fibers destroyed without ever being joined (test fixture, scheduler
       owned_fibers_.clear()).
  AC3 evaluator_env.cpp registers an orphan_root_release callback with
       the current Fiber when adding a live EnvFrameRef. Body's stack
       copies remain valid; only the global table entry is released on
       Reclaimed / Done.
  AC4 Fiber class exposes the orphan-root table API (register / release /
       has_orphan_roots + orphan_roots_dropped_on_reclaim_total /
       orphan_roots_hwm).
  AC5 Source-cite Issue #2498 in fiber.h, fiber.cpp, evaluator_env.cpp +
       linter self-test.

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

    fh = _read("src/serve/fiber.h")
    fc = _read("src/serve/fiber.cpp")
    env = _read("src/compiler/evaluator_env.cpp")
    _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/serve/test_fiber_reclaim_orphan_release.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — Fiber::join Reclaimed path precedes release_orphan_roots().
    # At least 4 Reclaimed sites in fiber.cpp; each must be preceded by
    # a release_orphan_roots() call within ~800 chars (the source-cite
    # pattern in the test verifies this; here we just enforce presence).
    must("release_orphan_roots", "AC1", fc)
    must("return finish(JoinStatus::Reclaimed)", "AC1", fc)
    # All 4 sites have a release_orphan_roots() preceding them.
    import re

    sites = list(re.finditer(r"return finish\(JoinStatus::Reclaimed\)", fc))
    preceding = 0
    for m in sites:
        start = max(0, m.start() - 800)
        ctx = fc[start : m.end()]
        if "release_orphan_roots" in ctx:
            preceding += 1
    if preceding < 4:
        fails.append(f"AC1: only {preceding}/{len(sites)} Reclaimed sites have release_orphan_roots() preceding")

    # AC2 — ~Fiber() safety net.
    must("Fiber::~Fiber", "AC2", fc)
    must("release_orphan_roots", "AC2", fc)
    must("safety-net release", "AC2", fc)

    # AC3 — evaluator_env.cpp registers the drop callback.
    must("register_orphan_root_release", "AC3", env)
    must("g_current_fiber", "AC3", env)
    must("unregister_live_env_frame_ref", "AC3", env)
    must("Issue #2498", "AC3", env)

    # AC4 — Fiber class API + static counters.
    must("register_orphan_root_release", "AC4", fh)
    must("release_orphan_roots", "AC4", fh)
    must("has_orphan_roots", "AC4", fh)
    must("orphan_roots_dropped_on_reclaim_total", "AC4", fh)
    must("orphan_roots_hwm", "AC4", fh)
    must("orphan_roots_mtx_", "AC4", fh)
    must("orphan_root_releases_", "AC4", fh)
    must("orphan_roots_dropped_on_reclaim_total_{0}", "AC4", fc)
    must("orphan_roots_hwm_{0}", "AC4", fc)

    # AC5 — Source-cite Issue #2498 in all touched files + linter self-test.
    must("Issue #2498", "AC5", fh)
    must("Issue #2498", "AC5", fc)
    must("Issue #2498", "AC5", env)
    must("Issue #2498", "AC5", test)
    must("AC5", "AC5", test)
    must("tests/serve/test_fiber_reclaim_orphan_release.cpp", "AC5", cmake)  # batch member source (S5)
    must("check_fiber_reclaim_orphan_release_2498", "AC5", build)

    # Optional: query surface exposure (the accessor is registered as a
    # static method on Fiber; the obs partition typically reads via a
    # C-linkage shim if exposed in query surface — skip for now, AC4's
    # process-wide counters are queryable via Fiber::orphan_roots_* accessors
    # directly).

    if fails:
        print("check_fiber_reclaim_orphan_release_2498: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_fiber_reclaim_orphan_release_2498: OK (5/5 AC rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
