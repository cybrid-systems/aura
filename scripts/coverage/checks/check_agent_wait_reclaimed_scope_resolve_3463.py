#!/usr/bin/env python3
# scripts/check_agent_wait_reclaimed_scope_resolve_3463.py — Issue #3463 source-cite gate.
#
# Routes orch:agent-wait-reclaimed (and its :abandon arm) through the
# existing #3442 resolve_aura_agent helper (name-table first, then
# AgentScope::find on the same Evaluator). Closes the residual gap that
# a host following the documented scope-spawn path (cleanup-pending /
# must_wait_reclaimed) could not reach the SSOT second-wait by name
# from Aura. C++ hosts holding AgentScope::handles_mut() are unchanged.
#
# Contract:
#   AC1  orch:agent-wait-reclaimed routes through resolve_aura_agent
#        (NOT direct agent_names_->find), with a #3463 stamp
#   AC2  resolve_aura_agent retains the #3442 shape (name-table first,
#        then find_agent_scope pointer walk, then scope->find)
#   AC3  Aura negative path: orch:agent-wait-reclaimed on unknown name
#        → status=invalid + wired sentinel (proves prim path is alive)
#   AC4  No new query:* key; reuse wait_reclaimed_* /
#        reclaimed_abandon_total / cleanup-pending. No
#        test_issue_3463.cpp (per #81967). No docs/design/3463-*
#        (per #1655). No process-global AgentRegistry (issue non-goal).
#   AC5  build.py wires the linter; linter lives under
#        scripts/coverage/checks/ (pre-push regression-guard location).
#   AC6  C++ hosts holding AgentScope::handles_mut() are unchanged
#        (ensure_reclaimed_cleanup / wait_reclaimed_body still handle-based).
#
# Default: --strict. CI gate.
#
# Self-test:
#   python3 scripts/check_agent_wait_reclaimed_scope_resolve_3463.py --self-test
#
# Catches the regression where orch:agent-wait-reclaimed falls back to
# the raw agent_names_->find only (pre-#3463 shape), which would
# silently return status=invalid for scope-spawn agents that already
# owe Reclaimed cleanup — the #3442 message-plane split closes but the
# cleanup-pending / must_wait_reclaimed fail-closed path stays
# unreachable from Aura.

from __future__ import annotations

import argparse
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

PRIM = "src/compiler/evaluator_primitives_agent.cpp"
SCOPE = "src/orch/agent_scope.h"
SPAWN = "src/orch/agent_spawn.h"
TEST_JOIN_DRAIN = "tests/orch/test_join_drain_reclaim.cpp"
BUILD_PY = "build.py"
LINTER_PATH = "scripts/coverage/checks/check_agent_wait_reclaimed_scope_resolve_3463.py"

# ── required patterns ──────────────────────────────────────────────────────
PRIM_REQUIRED: tuple[str, ...] = (
    # AC1: the second-wait prim routes through resolve_aura_agent.
    "Issue #3463",
    "resolve_aura_agent(ev, name)",
    # AC2: resolve_aura_agent retains the #3442 shape.
    "aura::orch::AgentHandle* resolve_aura_agent(Evaluator& ev, const std::string& name)",
    "if (auto* h = ev.agent_names_->find(name))",
    "if (auto* scope = aura::orch::find_agent_scope(static_cast<void*>(&ev)))",
    "return scope->find(name);",
    # AC4: existing keys reused; no new query:* primitive.
    "wait-reclaimed-total",
    "reclaimed-abandon-total",
    "cleanup-pending",
)

PRIM_FORBIDDEN: tuple[str, ...] = (
    # AC4: no new query key.
    '"query:agent-wait-reclaimed"',
    '"query:orch-agent-wait-reclaimed"',
    '"query:reclaimed-wait"',
)

SCOPE_REQUIRED: tuple[str, ...] = (
    # AC6: AgentScope::find retained (per #3463 body — "find still
    # returns the handle after join_all").
    "find_agent_scope",
)

SPAWN_FORBIDDEN: tuple[str, ...] = (
    # Non-goal: do NOT reintroduce a process-global AgentRegistry.
    "class AgentRegistry",
    "struct AgentRegistry",
)

TEST_REQUIRED: tuple[str, ...] = (
    # AC1+AC2+AC3+AC4 tests live in test_join_drain_reclaim.cpp.
    "ac3463_1_source_cite_routes_through_resolve_aura_agent",
    "ac3463_2_resolve_aura_agent_unchanged",
    "ac3463_3_aura_negative_path_unknown_name",
    "ac3463_4_no_new_query_key_and_no_invent",
    # AC1 source-cite shape.
    "3463 AC1: orch:agent-wait-reclaimed uses resolve_aura_agent",
    # AC4 no-invent (#81967 + #1655).
    "3463 AC4: no test_issue_3463.cpp per #81967",
    "3463 AC4: no docs/design/3463-* per #1655",
)

BUILD_REQUIRED: tuple[str, ...] = (
    "check_agent_wait_reclaimed_scope_resolve_3463",
    "Issue #3463 wait-reclaimed scope-resolve linter failed",
)


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


def check_wait_reclaimed_prim_window(text: str) -> list[str]:
    """Windowed check: inside the orch:agent-wait-reclaimed prim body,
    the raw agent_names_->find(name) call must NOT appear. Other
    prims (orch:agent-touch, orch:agent-poll, orch:agent-export-via-token)
    legitimately use the raw find — those are unrelated.
    """
    problems: list[str] = []
    start = text.find('add("orch:agent-wait-reclaimed"')
    if start < 0:
        problems.append(f"{PRIM}: orch:agent-wait-reclaimed prim not found")
        return problems
    # Window: from the prim's add( to the next add( at the same depth
    # (or 4000 chars, whichever comes first). Match brace depth to
    # close the lambda.
    depth = 0
    end = start
    saw_open = False
    for i in range(start, min(start + 6000, len(text))):
        c = text[i]
        if c == "{":
            depth += 1
            saw_open = True
        elif c == "}":
            depth -= 1
            if saw_open and depth == 0:
                end = i + 1
                break
    if end <= start:
        end = min(start + 4000, len(text))
    body = text[start:end]
    # The pre-#3463 bug shape: raw find inside the second-wait prim.
    if "hp = ev.agent_names_->find(name);" in body:
        problems.append(
            f"{PRIM}: orch:agent-wait-reclaimed body still uses raw "
            f"agent_names_->find (should route through resolve_aura_agent)"
        )
    # Positive: resolve_aura_agent called inside the prim body.
    if "resolve_aura_agent(ev, name)" not in body:
        problems.append(f"{PRIM}: orch:agent-wait-reclaimed body missing resolve_aura_agent(ev, name) call site")
    return problems


def run_checks() -> list[str]:
    problems: list[str] = []
    problems += check_file(PRIM, PRIM_REQUIRED, PRIM_FORBIDDEN)
    # Windowed check on the second-wait prim specifically — global
    # forbidden patterns would false-positive on orch:agent-touch /
    # orch:agent-poll / orch:agent-export-via-token which legitimately
    # use the raw find (their contract is name-table-only).
    problems += check_wait_reclaimed_prim_window(read_repo_file(PRIM))
    problems += check_file(SCOPE, SCOPE_REQUIRED)
    problems += check_file(SPAWN, (), SPAWN_FORBIDDEN)
    problems += check_file(TEST_JOIN_DRAIN, TEST_REQUIRED)
    problems += check_file(BUILD_PY, BUILD_REQUIRED)
    # AC4: tests live in src-aligned suites — no test_issue_3463.cpp.
    for stray in ("tests/orch/test_issue_3463.cpp", "tests/issues/test_issue_3463.cpp"):
        if (REPO_ROOT / stray).exists():
            problems.append(f"{stray}: test_issue_NNNN.cpp files are forbidden (AC4)")
    # AC4: no docs/design/3463-*.md per #1655.
    docs_dir = REPO_ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("3463-*")):
            problems.append(f"docs/design/{f.name}: docs/design/<NNNN>-* forbidden (#1655)")
    # AC5: linter lives under scripts/coverage/checks/.
    if not (REPO_ROOT / LINTER_PATH).is_file():
        problems.append(f"{LINTER_PATH}: linter must live under scripts/coverage/checks/")
    return problems


# ── self-test ──────────────────────────────────────────────────────────────
# Feed the detector a synthetic regression (raw agent_names_->find in the
# second-wait prim + a new query key) and assert both are caught.
SELFTEST_BAD_PRIM = """\
aura::orch::AgentHandle* resolve_aura_agent(Evaluator& ev, const std::string& name) {
    if (ev.agent_names_) {
        if (auto* h = ev.agent_names_->find(name))
            return h;
    }
    if (auto* scope = aura::orch::find_agent_scope(static_cast<void*>(&ev)))
        return scope->find(name);
    return nullptr;
}
// Issue #3463: bad regression
add("orch:agent-wait-reclaimed", [](auto a) {
    hp = ev.agent_names_->find(name);  // regression: raw find outside resolve
    return ...;
});
// new query key regression
add("query:agent-wait-reclaimed", [](auto a) { return make_int(0); });
"""


def self_test() -> int:
    ok = True
    with tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False) as f:
        f.write(SELFTEST_BAD_PRIM)
        bad = f.name
    # 1. Synthetic prim has the regression: raw find outside resolve + new query key.
    prim_problems = check_file(bad, (), PRIM_FORBIDDEN)
    if not prim_problems or any("hp = ev.agent_names_->find(name);" not in p for p in prim_problems):
        print("self-test: detector missed the raw-find regression", file=sys.stderr)
        ok = False
    else:
        print("self-test: raw-find regression detected ✓")
    if not any("query:agent-wait-reclaimed" in p for p in prim_problems):
        print("self-test: detector missed the new query key", file=sys.stderr)
        ok = False
    else:
        print("self-test: new query key regression detected ✓")
    # 2. Real repo passes.
    real = run_checks()
    if real:
        ok = False
        for p in real:
            print(f"self-test: real-repo check failed: {p}", file=sys.stderr)
    else:
        print("self-test: real-repo checks pass ✓")
    Path(bad).unlink(missing_ok=True)
    return 0 if ok else 1


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Issue #3463 source-cite gate")
    parser.add_argument("--self-test", action="store_true", help="run the built-in regression self-test")
    parser.add_argument("--strict", action="store_true", default=True, help="exit non-zero on any finding")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    problems = run_checks()
    if problems:
        for p in problems:
            print(f"check_agent_wait_reclaimed_scope_resolve_3463: FAIL {p}", file=sys.stderr)
        return 1
    print("check_agent_wait_reclaimed_scope_resolve_3463: OK (#3463 gates present)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
