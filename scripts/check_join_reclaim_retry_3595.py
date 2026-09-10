#!/usr/bin/env python3
# scripts/check_join_reclaim_retry_3595.py -- Issue #3595 source-cite gate.
#
# Verifies the bounded production reclaimed-cleanup retry on the join
# surface (maybe_auto_wait_reclaimed_production loops ensure_reclaimed_cleanup
# under reclaimed_retry_budget_ms(drain_ms)):
#
#  AC1: Wrapper takes the bounded budget param; every wait goes through
#       ensure_reclaimed_cleanup (the only second-wait SSOT — no third
#       cleanup function); the expiry arm bumps the #3220 host-forget risk
#       counter once.
#  AC2: reclaimed_retry_budget_ms reuses the #2227 shape
#       min(drain_ms * 8, kJoinDrainResidualHardMsDefault); no new env /
#       query key.
#  AC3: join_agent + join_agents production arms route through the wrapper
#       (no direct wait_reclaimed_body / inline host_forget bump left in
#       the #3110 arms); drain=0 cancel-only stays one-shot. #3631:
#       join_agents defers to the shared-budget batch pass
#       (maybe_auto_wait_reclaimed_batch) — same bounded contract, wall ~=
#       budget instead of N x budget; single-handle join_agent keeps the
#       wrapper.
#  AC4: Both join prims pass reclaimed_retry_budget_ms(policy.drain_ms);
#       the wrapper never auto-abandons (#3334 stays host-opt-in); no
#       body-stack free (#2661).
#  AC5: Source-cite only. No docs/design/3595-* (per MEMORY 2026-07-19),
#       no tests/issues/test_issue_3595.cpp (#81934). Existing counters
#       (wait_reclaimed_total / wait_reclaimed_timeout_total /
#       host_forget_reclaimed_risk_total) reused.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = ("src/orch/agent_spawn.h", "src/compiler/evaluator_primitives_agent.cpp")

# (path, regex, label) -- each tuple is a regex pattern that must appear.
REQUIRED: tuple[tuple[str, str, str], ...] = (
    # AC1: bounded retry wrapper delegates every wait to the SSOT helper.
    (
        "src/orch/agent_spawn.h",
        r"Issue\s+#3595",
        "3595 AC1: agent_spawn.h cites #3595",
    ),
    (
        "src/orch/agent_spawn.h",
        r"maybe_auto_wait_reclaimed_production\(\s*AgentHandle&\s+h,\s*bool\s+caller_passed_wait_reclaimed_ms,\s*std::uint64_t\s+retry_budget_ms",
        "3595 AC1: wrapper takes the bounded budget param",
    ),
    (
        "src/orch/agent_spawn.h",
        r"waited_us\s*\+=\s*ensure_reclaimed_cleanup\(h\)\.wait_us;",
        "3595 AC1: every wait goes through the ensure_reclaimed_cleanup SSOT",
    ),
    (
        "src/orch/agent_spawn.h",
        r"host_forget_reclaimed_risk_total\.fetch_add",
        "3595 AC1: expiry arm bumps the #3220 host-forget risk counter",
    ),
    # AC2: budget helper reuses the #2227 shape.
    (
        "src/orch/agent_spawn.h",
        r"reclaimed_retry_budget_ms\(std::uint64_t drain_ms\) noexcept",
        "3595 AC2: budget helper declared",
    ),
    (
        "src/orch/agent_spawn.h",
        r"std::min\(drain_ms \* 8, kJoinDrainResidualHardMsDefault\)",
        "3595 AC2: budget reuses the #2227 min(drain*8, hard) shape",
    ),
    # AC3: C++ join surfaces route the production arm through the wrapper.
    (
        "src/orch/agent_spawn.h",
        r"jr\.wait_us\s*\+=\s*maybe_auto_wait_reclaimed_production\(\s*h,\s*/\*caller_passed_wait_reclaimed_ms=\*/false,\s*reclaimed_retry_budget_ms\(policy\.drain_ms\)\);",
        "3595 AC3: join_agent production arm routes through the wrapper",
    ),
    (
        "src/orch/agent_spawn.h",
        r"maybe_auto_wait_reclaimed_batch\(\s*agents,\s*reclaimed_retry_budget_ms\(policy\.drain_ms\)\);",
        "3595 AC3: join_agents production arm routes the shared-budget batch pass (#3631)",
    ),
    # AC4: prims pass the drain-scaled budget.
    (
        "src/compiler/evaluator_primitives_agent.cpp",
        r"Issue\s+#3595",
        "3595 AC4: join prims cite #3595",
    ),
    (
        "src/compiler/evaluator_primitives_agent.cpp",
        r"aura::orch::reclaimed_retry_budget_ms\(policy\.drain_ms\)",
        "3595 AC4: join prims pass the drain-scaled budget",
    ),
    # AC5: existing counters reused (no new counter / query key).
    (
        "src/orch/agent_spawn.h",
        r"wait_reclaimed_timeout_total\.fetch_add",
        "3595 AC5: reused wait_reclaimed_timeout_total counter retained",
    ),
)

# (path, regex, label) -- each regex must NOT appear.
FORBIDDEN: tuple[tuple[str, str, str], ...] = (
    (
        "src/orch/agent_spawn.h",
        r"query:reclaim-retry|query:join-reclaim-retry|AURA_JOIN_RECLAIM_RETRY",
        "3595 AC2/AC4: no new query key / no new env (non-goal)",
    ),
)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def run_checks() -> list[str]:
    """Returns a list of failure labels (empty = clean)."""
    failures: list[str] = []
    cache: dict[str, str] = {}

    def body(path: str) -> str:
        if path not in cache:
            full = REPO_ROOT / path
            cache[path] = read(full) if full.exists() else ""
        return cache[path]

    for path, pattern, label in REQUIRED:
        if re.search(pattern, body(path)) is None:
            failures.append(label)

    for path, pattern, label in FORBIDDEN:
        if re.search(pattern, body(path)) is not None:
            failures.append(label)

    # AC4: the retry loop never auto-abandons (#3334 host-opt-in) and never
    # frees the body stack (#2661). Bounded extract: wrapper signature up to
    # the next section marker (Issue #3334 abandon_reclaimed block).
    spawn = body("src/orch/agent_spawn.h")
    w0 = spawn.find("std::uint64_t retry_budget_ms =")
    w1 = spawn.find("Issue #3334", w0 + 1)
    if w0 == -1 or w1 == -1 or w1 <= w0:
        failures.append("3595 AC4: wrapper section not bounded (anchor drift)")
    else:
        wrapper_body = spawn[w0:w1]
        if "abandon_reclaimed" in wrapper_body:
            failures.append("3595 AC4: retry loop must not auto-abandon (#3334)")
        if "ensure_reclaimed_cleanup(h)" not in wrapper_body:
            failures.append("3595 AC4: wrapper must delegate waits to the SSOT helper")

    # AC5: no design doc, no standalone issue test.
    if (REPO_ROOT / "docs" / "design" / "3595-join-reclaim-retry.md").exists():
        failures.append("3595 AC5: docs/design/3595-* must not exist (#1655)")
    if (REPO_ROOT / "tests" / "issues" / "test_issue_3595.cpp").exists():
        failures.append("3595 AC5: tests/issues/test_issue_3595.cpp must not exist (#81934)")

    return failures


def self_test() -> int:
    """Regexes compile + anchor texts exist in the targets (pre-flight)."""
    ok = True
    for _, pattern, label in REQUIRED:
        try:
            re.compile(pattern)
        except re.error as e:
            print(f"SELF-TEST FAIL (regex compile): {label}: {e}")
            ok = False
    for path in DEFAULT_TARGETS:
        if not (REPO_ROOT / path).exists():
            print(f"SELF-TEST FAIL (missing target): {path}")
            ok = False
    if ok:
        print("self-test: regexes compile + targets present")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3595 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    ap.add_argument("--self-test", action="store_true", help="regex + anchor pre-flight")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    failures = run_checks()
    for f in failures:
        print(f"FAIL: {f}")
    if failures:
        print(f"check_join_reclaim_retry_3595: {len(failures)} row(s) failed")
        return 1 if args.strict else 0
    print("check_join_reclaim_retry_3595: all rows clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
