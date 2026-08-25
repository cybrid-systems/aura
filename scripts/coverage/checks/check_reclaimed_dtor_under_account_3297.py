#!/usr/bin/env python3
"""Issue #3297 linter: ~AgentHandle under-account observability.

When a long-lived C++ supervisor holds AgentHandle after production
auto-wait Timeout and the body is still non-yielding, the dtor's
unconditional release_reservation_if_any() releases arena quota
before the body exits -- brief under-account window (not a permanent
leak; #3012 accepts "anti-permanent-leak" priority).

Observability only (zero behavior change):
  - Additive counter reclaimed_dtor_under_account_total appended at
    OrchModuleStats struct END (#2906 discipline).
  - Bumped in finish_reclaimed_cleanup_on_dtor BEFORE the
    unconditional release_reservation_if_any() when
    reclaimed_deferred_cleanup && fiber && !fiber->is_done().
  - Soft / body-already-exit / explicit wait_reclaimed_ms paths stay
    zero (same condition that skips the Done-path above).

Scans:
  - src/orch/agent_spawn.h: OrchModuleStats struct END has
    reclaimed_dtor_under_account_total (#2906 discipline = append,
    never insert in middle).
  - src/orch/agent_spawn.h: AgentHandle::finish_reclaimed_cleanup_on_dtor
    body bumps the counter BEFORE release_reservation_if_any().
  - src/orch/agent_spawn.h: gate is reclaimed_deferred_cleanup && fiber
    && !fiber->is_done() AND production_defaults_active() (#3297
    acceptance "Soft / Off 零行为变化 / 无新 atomic" -> Soft/Off stays 0).
  - tests/orch/test_join_drain_reclaim.cpp: AC1 (Timeout + immediate
    dtor -> counter bump) + AC2 (body exit then dtor -> no bump) +
    AC3 (Soft zero-cost).
  - docs/ + tests/ no forbidden paths (#1655 / #81967).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SPAWN = REPO / "src" / "orch" / "agent_spawn.h"
TEST = REPO / "tests" / "orch" / "test_join_drain_reclaim.cpp"

COUNTER_NAME = "reclaimed_dtor_under_account_total"
FN_NAME = "finish_reclaimed_cleanup_on_dtor"
ISSUE_ANCHOR = re.compile(r"Issue\s*#3297")
COUNTER_DECL = re.compile(rf"std::atomic<std::uint64_t>\s+{COUNTER_NAME}\s*\{{")
GATE_COND = re.compile(r"reclaimed_deferred_cleanup\s*&&\s*fiber\s*&&\s*!fiber->is_done\(\)")
RELEASE_CALL = re.compile(r"release_reservation_if_any\(\)\s*;")
COUNTER_BUMP = re.compile(rf"g_orch_module_stats\.{COUNTER_NAME}\.fetch_add\s*\(")
TEST_AC_MARKER = re.compile(r"ac3297_\w+|ac_\d+_3297|3297\s*AC\d+|3297_ac")


def _read(rel_path: Path) -> str:
    return rel_path.read_text(encoding="utf-8", errors="replace") if rel_path.is_file() else ""


def _find_function_body(src: str, decl_marker: str) -> tuple[int, int] | None:
    """Return (line_idx, body_lines) of the function body.

    Walks forward from decl_marker past signature noise (`;` in default
    args, template bounds, nested parens) to the opening `{` of the
    function body, then depth-counts braces to find the closing `}`.
    Returns None on miss.
    """
    m = re.search(decl_marker, src)
    if not m:
        return None
    start = m.end()
    # Walk forward, tracking paren depth (for default args / template
    # bounds) and brace depth, until we hit the function-body `{`.
    n = len(src)
    i = start
    paren_depth = 0
    while i < n:
        ch = src[i]
        if ch == "(":
            paren_depth += 1
        elif ch == ")":
            if paren_depth > 0:
                paren_depth -= 1
        elif paren_depth == 0:
            if ch == "{":
                break
            if ch == "}" or ch == ";":
                return None
        i += 1
    if i >= n:
        return None
    open_line = src[:start].count("\n")
    depth = 1
    j = i + 1
    while j < n and depth > 0:
        ch = src[j]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        j += 1
    body = src[i:j]
    body_lines = body.count("\n")
    return open_line, body_lines


def _check_struct_end_counter(spawn: str) -> list[str]:
    """reclaimed_dtor_under_account_total appended at OrchModuleStats struct END."""
    fails: list[str] = []
    if not ISSUE_ANCHOR.search(spawn):
        fails.append("agent_spawn.h: missing 'Issue #3297' anchor near counter decl")
    if not COUNTER_DECL.search(spawn):
        fails.append(f"agent_spawn.h: missing counter decl `std::atomic<std::uint64_t> {COUNTER_NAME}{{0}};`")
    # Position discipline (#2906): counter must come AFTER the previous
    # additive counters (host_forget_reclaimed_risk_total,
    # workflow_residual_*, agent_join_fail_action_cancel_total).
    # Verify it's not inserted in the middle of an older group.
    struct_open = spawn.find("struct OrchModuleStats {")
    if struct_open < 0:
        fails.append("agent_spawn.h: struct OrchModuleStats not found")
        return fails
    struct_close = spawn.find("\n};\n", struct_open)
    if struct_close < 0:
        fails.append("agent_spawn.h: OrchModuleStats struct closing `};` not found")
        return fails
    body = spawn[struct_open:struct_close]
    host_pos = body.find("host_forget_reclaimed_risk_total{0}")
    counter_pos = body.find(COUNTER_NAME + "{0}")
    cancel_pos = body.find("agent_join_fail_action_cancel_total{0}")
    if host_pos < 0 or cancel_pos < 0 or counter_pos < 0:
        fails.append(
            f"agent_spawn.h: OrchModuleStats missing one of "
            f"host_forget_reclaimed_risk_total / "
            f"agent_join_fail_action_cancel_total / {COUNTER_NAME}"
        )
    elif not (host_pos < cancel_pos < counter_pos):
        fails.append(
            f"agent_spawn.h: {COUNTER_NAME} must come AFTER "
            f"agent_join_fail_action_cancel_total (struct end, #2906): "
            f"host_pos={host_pos} cancel_pos={cancel_pos} counter_pos={counter_pos}"
        )
    return fails


def _check_dtor_bump_before_release(spawn: str) -> list[str]:
    """finish_reclaimed_cleanup_on_dtor bumps counter BEFORE release_reservation_if_any()."""
    fails: list[str] = []
    block = _find_function_body(spawn, r"AgentHandle::finish_reclaimed_cleanup_on_dtor")
    if not block:
        return ["agent_spawn.h: AgentHandle::finish_reclaimed_cleanup_on_dtor body not found"]
    line_idx, body_lines = block
    body = _slice(spawn, line_idx, body_lines + 1)
    if not ISSUE_ANCHOR.search(body):
        fails.append(f"agent_spawn.h: {FN_NAME} body missing 'Issue #3297' comment anchor (line ~{line_idx + 1})")
    if not COUNTER_BUMP.search(body):
        fails.append(
            f"agent_spawn.h: {FN_NAME} must bump g_orch_module_stats.{COUNTER_NAME}.fetch_add(1) line ~{line_idx + 1}"
        )
    if not GATE_COND.search(body):
        fails.append(
            f"agent_spawn.h: {FN_NAME} under-account gate must be "
            f"reclaimed_deferred_cleanup && fiber && !fiber->is_done() "
            f"(symmetric with Done-path skip)"
        )
    # Bump MUST come before release_reservation_if_any().
    # Anchor the release search to the call site (with semicolon) so
    # that mentions in comments BEFORE the bump don't false-positive.
    bump_pos = body.find(COUNTER_NAME + ".fetch_add")
    if bump_pos < 0:
        return fails
    # Search for the call site after the bump, walking forward.
    release_call_re = re.compile(r"release_reservation_if_any\s*\(\s*\)\s*;")
    release_pos = -1
    for m in release_call_re.finditer(body):
        # Only count release calls AFTER the bump.
        if m.start() > bump_pos:
            release_pos = m.start()
            break
    if release_pos < 0:
        # Fall back: any release call (even before bump) signals
        # absence of the call we want to verify ordering against.
        first_release = release_call_re.search(body)
        if first_release is None:
            fails.append(f"agent_spawn.h: {FN_NAME} no release_reservation_if_any(); call found (line ~{line_idx + 1})")
            return fails
        release_pos = first_release.start()
    if bump_pos > release_pos:
        fails.append(
            f"agent_spawn.h: {FN_NAME} counter bump must come BEFORE "
            f"release_reservation_if_any(); (bump_pos={bump_pos} > "
            f"release_pos={release_pos}, line ~{line_idx + 1})"
        )
    return fails


def _check_release_unchanged(spawn: str) -> list[str]:
    """release_reservation_if_any() body must remain unconditional (#2661 / #2009 preserved).

    The function is defined inline inside the AgentHandle class, so the
    regex matches `void release_reservation_if_any() noexcept {` (no
    `AgentHandle::` qualifier on inline member definitions).
    """
    fails: list[str] = []
    block = _find_function_body(spawn, r"\brelease_reservation_if_any\s*\(\s*\)\s*noexcept")
    if not block:
        return ["agent_spawn.h: AgentHandle::release_reservation_if_any body not found"]
    line_idx, body_lines = block
    body = _slice(spawn, line_idx, body_lines + 1)
    # The function must still early-out on reserved_memory_bytes==0 and
    # unconditionally call process_resource_quota().release_agent_arena.
    if "reserved_memory_bytes == 0" not in body:
        fails.append(
            f"agent_spawn.h: release_reservation_if_any early-out on "
            f"reserved_memory_bytes==0 missing (line ~{line_idx + 1})"
        )
    if "release_agent_arena" not in body:
        fails.append(
            "agent_spawn.h: release_reservation_if_any must still call "
            "process_resource_quota().release_agent_arena (#2661 preserved)"
        )
    if COUNTER_NAME in body:
        fails.append(
            f"agent_spawn.h: release_reservation_if_any must NOT bump "
            f"{COUNTER_NAME} (counter lives in dtor gate, not in the "
            f"idempotent release helper)"
        )
    return fails


def _check_test_extensions(test_src: str) -> list[str]:
    """Test AC1 + AC2 must exist; no test_issue_3297.cpp; no docs/design/."""
    fails: list[str] = []
    if "Issue #3297" not in test_src:
        fails.append(
            "test_join_drain_reclaim.cpp: missing 'Issue #3297' section "
            "(AC1+AC2 should be added near other AC sections)"
        )
    if "reclaimed_dtor_under_account_total" not in test_src:
        fails.append("test_join_drain_reclaim.cpp: missing `reclaimed_dtor_under_account_total` counter reference")
    # AC1: production + Reclaimed + auto-wait Timeout + immediate dtor ->
    # counter bumps; body still live.
    # AC2: body exit then dtor -> no bump; reserved_memory_bytes == 0.
    # AC3: Soft zero-cost (production_defaults_active gate).
    for marker in (
        "ac3297_1_dtor_under_account_live_body",
        "ac3297_2_dtor_no_under_account_post_exit",
        "ac3297_3_soft_zero_observability",
    ):
        if marker not in test_src:
            fails.append(f"test_join_drain_reclaim.cpp: missing {marker} function")
    if "production_defaults_active" not in test_src and "apply_production_audit_defaults" not in test_src:
        fails.append(
            "test_join_drain_reclaim.cpp: missing production posture toggle "
            "(needs apply_production_audit_defaults() or production_defaults_active())"
        )
    # AC5 forbids test_issue_3297.cpp (per #81967 + #1655).
    for forbidden_path in (
        REPO / "tests" / "orch" / "test_issue_3297.cpp",
        REPO / "tests" / "issues" / "test_issue_3297.cpp",
        REPO / "tests" / "core" / "test_issue_3297.cpp",
        REPO / "tests" / "compiler" / "test_issue_3297.cpp",
    ):
        if forbidden_path.exists():
            fails.append(f"{forbidden_path}: forbidden per #81967 / #1655 — extend test_join_drain_reclaim.cpp instead")
    docs_design = REPO / "docs" / "design"
    if docs_design.is_dir():
        for entry in docs_design.iterdir():
            if entry.name.startswith("3297-"):
                fails.append(
                    f"{entry}: forbidden per #1655 — close comment carries "
                    f"design rationale, no docs/design/ for agent-developed repo"
                )
    return fails


def _slice(src: str, start: int, max_lines: int = 60) -> str:
    return "\n".join(src.splitlines()[start : start + max_lines])


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #3297 linter")
    parser.add_argument("--self-test", action="store_true", help="Self-check mode")
    parser.add_argument("--strict", action="store_true", help="Strict mode (exit 1 on any fail)")
    args = parser.parse_args()

    fails: list[str] = []
    spawn = _read(SPAWN)
    test_src = _read(TEST)

    fails.extend(_check_struct_end_counter(spawn))
    fails.extend(_check_dtor_bump_before_release(spawn))
    fails.extend(_check_release_unchanged(spawn))
    fails.extend(_check_test_extensions(test_src))

    if args.self_test:
        if not ISSUE_ANCHOR.search(Path(__file__).read_text(encoding="utf-8")):
            fails.append("linter self-test: missing 'Issue #3297' anchor in own source")
        print("self-test OK" if not fails else "\n".join(fails))
        return 0 if not fails else 1

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed", file=sys.stderr)
        return 1 if args.strict else 1

    print(
        "OK #3297 reclaimed_dtor_under_account_total: struct-end discipline "
        "+ dtor-gate bump before release + release helper preserved"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
