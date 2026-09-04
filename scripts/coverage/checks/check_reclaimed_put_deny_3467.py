#!/usr/bin/env python3
# scripts/check_reclaimed_put_deny_3467.py — Issue #3467 source-cite gate.
#
# Verifies the closed-loop name-reuse deny for reclaimed-pending agent
# slots (orch/join residual-gap, High item C of the 2026-08-30 review):
#   AC1 — AgentNameTable::put typed deny over a pending slot
#         (must_wait_reclaimed / reclaimed_deferred_cleanup): no
#         move-assign, reservation stays held, nullptr returned.
#   AC2 — orch:spawn-agent pre-spawn deny: typed fail hash with the
#         existing #3220 lifecycle + #3251 deny-class keys (no new
#         query key), host_forget_reclaimed_risk_total bump, no spawn.
#   AC3 — orch:scope-join-all B1 strict drop: root slot dropped only
#         when every handle is settled (no live fiber, no pending
#         flags); pending handles never cleared (#2661).
#   AC4 — tests live in the src-aligned suites (no test_issue_3467.cpp).
#
# Default: --strict. CI gate.
#
# Self-test:
#   python3 scripts/check_reclaimed_put_deny_3467.py --self-test
#
# Catches regressions when a put/drop path is added that skips the
# pending gate (would re-open the #3297 under-account path from a
# normal same-name spawn — quota accounting and name identity diverge
# while a Reclaimed body is still running).

from __future__ import annotations

import argparse
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

NAME_TABLE = "src/compiler/agent_name_table.h"
AGENT_PRIMS = "src/compiler/evaluator_primitives_agent.cpp"
AGENT_SCOPE = "src/orch/agent_scope.h"
TEST_NAME_TABLE = "tests/orch/test_agent_name_table_isolation.cpp"
TEST_JOIN_DRAIN = "tests/orch/test_join_drain_reclaim.cpp"
TEST_ORCH_SCOPE = "tests/orch/test_orch_scope.cpp"

# ── required patterns ──────────────────────────────────────────────────────
NAME_TABLE_REQUIRED: tuple[str, ...] = (
    # Put guard: typed deny before move-assign (#3467).
    "Issue #3467",
    "if (it->second.must_wait_reclaimed || it->second.reclaimed_deferred_cleanup)",
    "return nullptr;",
    "aura::orch::AgentHandle* put(",
)

AGENT_PRIMS_REQUIRED: tuple[str, ...] = (
    # Spawn-side pre-deny (AC1/AC2): fail closed before spawn, reuse the
    # existing #3220 lifecycle + #3251 deny-class keys, bump the existing
    # host_forget counter — no new query key.
    "Issue #3467",
    "name-reuse-while-reclaimed-pending",
    "AgentDenyClass::Other",
    "add_reclaimed_pending_lifecycle(rkv, /*pending=*/true)",
    "host_forget_reclaimed_risk_total",
    # Scope-join-all B1 guarded drop (AC3): root only; drop only when
    # all_settled. Issue #3496: settled is the TREE via tree_settled()
    # (live-fiber + pending flags live in agent_scope.h).
    "all_settled",
    "tree_settled()",
    "if (scope == root)",
)

# Issue #3496: the live-fiber + both-pending-flag walk is tree_settled
# (join_all stays local; drop sees descendants).
AGENT_SCOPE_REQUIRED: tuple[str, ...] = (
    "kJoinAllTreeSettledIssue = 3496",
    "tree_settled_unlocked_",
    "(hp.fiber && !hp.fiber->is_done())",
    "hp.must_wait_reclaimed",
    "hp.reclaimed_deferred_cleanup",
)

# The old unguarded drop must be gone (comment-vs-code mismatch that
# #3467 closes).
AGENT_PRIMS_FORBIDDEN: tuple[str, ...] = ("if (scope == root && scope->empty())",)

TEST_REQUIRED: dict[str, tuple[str, ...]] = {
    TEST_NAME_TABLE: (
        "ac3467_put_deny_pending",
        "3467 AC1: put over must_wait slot returns nullptr (typed deny)",
        "3467 AC2: clean slot still replaces",
        "3467 AC5: put allowed after cleanup (flags cleared)",
    ),
    TEST_JOIN_DRAIN: (
        "ac3467_name_reuse_fail_closed",
        "3467 AC1: put over pending slot denied (nullptr)",
        "3467 AC1: no reservation release on old handle",
        "3467 AC1: no reclaimed_dtor_under_account bump from deny",
        "3467 AC5: put allowed after cleanup (flags cleared)",
    ),
    TEST_ORCH_SCOPE: (
        "3467 AC4: directory empty after settled join-all (slot dropped)",
        "3467 AC4: scope-resolve misses after settled join-all",
        "3467 AC4: drop gate checks both pending flags",
        "3467 AC4 / #3496: drop uses tree_settled (descendants)",
    ),
}


def read_repo_file(rel: str) -> str:
    return (REPO_ROOT / rel).read_text(encoding="utf-8")


def check_file(path: str, required: Sequence[str], forbidden: Sequence[str] = ()) -> list[str]:
    problems: list[str] = []
    try:
        text = read_repo_file(path)
    except OSError as exc:
        return [f"{path}: unreadable ({exc})"]
    for pat in required:
        if pat not in text:
            problems.append(f"{path}: missing required pattern: {pat!r}")
    for pat in forbidden:
        if pat in text:
            problems.append(f"{path}: forbidden pattern present: {pat!r}")
    return problems


def run_checks() -> list[str]:
    problems: list[str] = []
    problems += check_file(NAME_TABLE, NAME_TABLE_REQUIRED)
    problems += check_file(AGENT_PRIMS, AGENT_PRIMS_REQUIRED, AGENT_PRIMS_FORBIDDEN)
    problems += check_file(AGENT_SCOPE, AGENT_SCOPE_REQUIRED)
    for path, required in TEST_REQUIRED.items():
        problems += check_file(path, required)
    # AC4: tests live in src-aligned suites — no test_issue_3467.cpp.
    for stray in ("tests/orch/test_issue_3467.cpp", "tests/issues/test_issue_3467.cpp"):
        if (REPO_ROOT / stray).exists():
            problems.append(f"{stray}: test_issue_NNNN.cpp files are forbidden (AC4)")
    return problems


# ── self-test ──────────────────────────────────────────────────────────────
# Feed the detector a synthetic regression (old unguarded put + unguarded
# drop) and assert it is caught.
SELFTEST_BAD_PUT = """\
aura::orch::AgentHandle& put(aura::orch::AgentHandle h) {
    auto [it, inserted] = impl_->agents_.try_emplace(name, std::move(h));
    if (!inserted) {
        it->second = std::move(h);
    }
    return it->second;
}
"""
SELFTEST_BAD_DROP = "if (scope == root && scope->empty())\n    drop_agent_scope(key);"


def self_test() -> int:
    ok = True
    with tempfile.NamedTemporaryFile("w", suffix=".h", delete=False) as f:
        f.write(SELFTEST_BAD_PUT)
        bad_put = f.name
    with tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False) as f:
        f.write(SELFTEST_BAD_DROP)
        bad_drop = f.name
    # 1. The synthetic put is missing the guard → detected.
    missing = [p for p in NAME_TABLE_REQUIRED if p not in Path(bad_put).read_text()]
    if "return nullptr;" not in "".join(missing):
        print("self-test: detector missed the unguarded put regression", file=sys.stderr)
        ok = False
    # 2. The synthetic drop hits the forbidden pattern → detected.
    if check_file(bad_drop, [], AGENT_PRIMS_FORBIDDEN):
        print("self-test: forbidden unguarded drop pattern detected ✓")
    else:
        print("self-test: detector missed the unguarded drop regression", file=sys.stderr)
        ok = False
    # 3. The real repo passes.
    real = run_checks()
    if real:
        ok = False
        for p in real:
            print(f"self-test: real-repo check failed: {p}", file=sys.stderr)
    else:
        print("self-test: real-repo checks pass ✓")
    # Cleanup temp files.
    Path(bad_put).unlink(missing_ok=True)
    Path(bad_drop).unlink(missing_ok=True)
    return 0 if ok else 1


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Issue #3467 source-cite gate")
    parser.add_argument("--self-test", action="store_true", help="run the built-in regression self-test")
    parser.add_argument("--strict", action="store_true", default=True, help="exit non-zero on any finding")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    problems = run_checks()
    if problems:
        for p in problems:
            print(f"check_reclaimed_put_deny_3467: FAIL {p}", file=sys.stderr)
        return 1
    print("check_reclaimed_put_deny_3467: OK (#3467 gates present)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
