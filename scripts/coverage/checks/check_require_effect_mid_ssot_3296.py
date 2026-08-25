#!/usr/bin/env python3
"""Issue #3296 linter: require_effect mid SSOT cascade order.

Production SSOT under Restricted/Strict is:
    TypedMid (boundary-stamped) -> current_mutation_epoch() -> 1
process_resource_quota_manager().provenance_mutation_id must NOT appear
in mid-resolution cascade paths; it can drift / lag relative to the
TypedMid that was live when the grant was issued (steal * abort *
boundary enter clears quota + epoch while TypedMid remains).

Scans:
  - src/compiler/evaluator_security.cpp    Evaluator::require_effect cascade
  - src/compiler/typed_mutation_audit.h    resolve_audit_mutation_id +
                                           composite_batch_join_mid cascades

Allowed exemption: process_resource_quota_manager().provenance_mutation_id
inside evaluator.ixx:13063 (mutation quota exceed log message — NOT a
mid-decision path; carries provenance_mutation_id only as diagnostic
context, paired with defuse_version fallback).

Source-cite AC1 + AC4:
  - TypedMid (last_type_linear_commit_proof_stamp_v_read) precedes
    current_mutation_epoch() in the cascade
  - 'Issue #3296' comment anchor present in all 3 production sites
  - AuditWalRecord.provenance_mutation_id + CapabilityGrant.bound_mutation_id
    + SecurityEvent.mutation_id join via check_and_record_effect's
    prov.mutation_id (single pass-through from require_effect mid)
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
EVAL_SEC = REPO / "src" / "compiler" / "evaluator_security.cpp"
TMA = REPO / "src" / "compiler" / "typed_mutation_audit.h"
EVAL_IXX = REPO / "src" / "compiler" / "evaluator.ixx"
TEST = REPO / "tests" / "compiler" / "test_require_effect_auto_isolation.cpp"

CASCADE_QUOTA_FORBID = re.compile(r"process_resource_quota_manager\(\)\.provenance_mutation_id")
EVAL_REQUIRE_EFFECT = re.compile(
    r"\bbool\s+Evaluator::require_effect\s*\(",
    re.MULTILINE,
)
RESOLVE_AUDIT_MID = re.compile(
    r"inline\s+std::uint64_t\s*\nresolve_audit_mutation_id\s*\(",
    re.MULTILINE,
)
COMPOSITE_BATCH_JOIN_MID = re.compile(
    r"\binline\s+std::uint64_t\s+pin_composite_batch_join_mid\s*\(",
    re.MULTILINE,
)
TYPED_MID_VREAD = re.compile(r"last_type_linear_commit_proof_stamp_v_read\(")
CURRENT_EPOCH = re.compile(r"::aura::core::current_mutation_epoch\(")
ISSUE_3296_ANCHOR = re.compile(r"Issue\s*#3296")


def _slice(src: str, start: int, max_lines: int = 60) -> str:
    return "\n".join(src.splitlines()[start : start + max_lines])


def _find_func_block(src: str, open_re: re.Pattern[str]) -> tuple[int, int] | None:
    """Return (line_idx, line_count) of the matched function body.

    Searches for `open_re` and walks forward from the opening `{` to the
    matching close brace (depth-counted). Returns None on miss.
    """
    m = open_re.search(src)
    if not m:
        return None
    start = m.start()
    # Walk forward to the opening brace of the function body.
    depth = 0
    i = m.end()
    n = len(src)
    while i < n:
        ch = src[i]
        if ch == "{":
            depth += 1
            break
        if ch == "}":
            return None
        i += 1
    open_line = src[:start].count("\n")
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


def _check_require_effect_cascade(esec: str) -> list[str]:
    """Evaluator::require_effect cascade: TypedMid BEFORE epoch, no quota."""
    fails: list[str] = []
    block = _find_func_block(esec, EVAL_REQUIRE_EFFECT)
    if not block:
        return ["evaluator_security.cpp: Evaluator::require_effect block not found"]
    line_idx, body_lines = block
    body = _slice(esec, line_idx, body_lines + 1)
    if CASCADE_QUOTA_FORBID.search(body):
        fails.append(
            f"evaluator_security.cpp: Evaluator::require_effect cascade MUST NOT use "
            f"process_resource_quota_manager().provenance_mutation_id under production "
            f"(line ~{line_idx + 1}); SSOT is TypedMid -> epoch -> 1 (#3296 AC1)"
        )
    tm_pos = body.find("last_type_linear_commit_proof_stamp_v_read")
    ep_pos = body.find("::aura::core::current_mutation_epoch")
    if tm_pos < 0 or ep_pos < 0:
        fails.append(
            f"evaluator_security.cpp: require_effect cascade missing TypedMid/anchor "
            f"(tm_pos={tm_pos}, ep_pos={ep_pos}, line ~{line_idx + 1})"
        )
    elif tm_pos > ep_pos:
        fails.append(
            f"evaluator_security.cpp: TypedMid must PRECEDE epoch in require_effect "
            f"cascade (tm_pos={tm_pos} > ep_pos={ep_pos}, line ~{line_idx + 1})"
        )
    if not ISSUE_3296_ANCHOR.search(body):
        fails.append(
            f"evaluator_security.cpp: require_effect cascade missing 'Issue #3296' "
            f"comment anchor (line ~{line_idx + 1})"
        )
    return fails


def _check_resolve_audit_mid(tma: str) -> list[str]:
    """resolve_audit_mutation_id cascade: caller_mid -> TypedMid -> epoch -> refuse.

    Quota MUST NOT appear in this cascade under production.
    """
    fails: list[str] = []
    block = _find_func_block(tma, RESOLVE_AUDIT_MID)
    if not block:
        return ["typed_mutation_audit.h: resolve_audit_mutation_id block not found"]
    line_idx, body_lines = block
    body = _slice(tma, line_idx, body_lines + 1)
    if CASCADE_QUOTA_FORBID.search(body):
        fails.append(
            f"typed_mutation_audit.h: resolve_audit_mutation_id cascade MUST NOT use "
            f"process_resource_quota_manager().provenance_mutation_id under production "
            f"(line ~{line_idx + 1}); SSOT is caller_mid -> TypedMid -> epoch -> refuse "
            f"(#3296 AC1)"
        )
    tm_pos = body.find("last_type_linear_commit_proof_stamp_v_read")
    ep_pos = body.find("::aura::core::current_mutation_epoch")
    if tm_pos < 0 or ep_pos < 0:
        fails.append(
            f"typed_mutation_audit.h: resolve_audit_mutation_id cascade missing "
            f"TypedMid/anchor (tm_pos={tm_pos}, ep_pos={ep_pos}, line ~{line_idx + 1})"
        )
    elif tm_pos > ep_pos:
        fails.append(
            f"typed_mutation_audit.h: TypedMid must PRECEDE epoch in "
            f"resolve_audit_mutation_id cascade (tm_pos={tm_pos} > ep_pos={ep_pos}, "
            f"line ~{line_idx + 1})"
        )
    if not ISSUE_3296_ANCHOR.search(body):
        fails.append(
            f"typed_mutation_audit.h: resolve_audit_mutation_id missing 'Issue #3296' "
            f"comment anchor (line ~{line_idx + 1})"
        )
    return fails


def _check_composite_batch_join_mid(tma: str) -> list[str]:
    """composite_batch_join_mid cascade: TypedMid before minting new id.

    Quota MUST NOT appear; if TypedMid == 0 && epoch == 0, mint via
    next_audit_mutation_id (existing behavior).
    """
    fails: list[str] = []
    block = _find_func_block(tma, COMPOSITE_BATCH_JOIN_MID)
    if not block:
        return ["typed_mutation_audit.h: composite_batch_join_mid block not found"]
    line_idx, body_lines = block
    body = _slice(tma, line_idx, body_lines + 1)
    if CASCADE_QUOTA_FORBID.search(body):
        fails.append(
            f"typed_mutation_audit.h: composite_batch_join_mid cascade MUST NOT use "
            f"process_resource_quota_manager().provenance_mutation_id under production "
            f"(line ~{line_idx + 1}); SSOT replaces quota with TypedMid (#3296 AC1)"
        )
    tm_pos = body.find("last_type_linear_commit_proof_stamp_v_read")
    if tm_pos < 0:
        fails.append(
            f"typed_mutation_audit.h: composite_batch_join_mid missing TypedMid anchor "
            f"(line ~{line_idx + 1}); SSOT is TypedMid -> epoch -> next_audit_mutation_id "
            f"(#3296 AC1)"
        )
    if not ISSUE_3296_ANCHOR.search(body):
        fails.append(
            f"typed_mutation_audit.h: composite_batch_join_mid missing 'Issue #3296' "
            f"comment anchor (line ~{line_idx + 1})"
        )
    return fails


def _check_test_extensions(test_src: str) -> list[str]:
    """Test AC1-AC5 must exist + AC6 forbids test_issue_3296.cpp + docs/design/."""
    fails: list[str] = []
    if "Issue #3296" not in test_src:
        fails.append(
            "test_require_effect_auto_isolation.cpp: missing 'Issue #3296' section "
            "(AC1-AC5 should be added near other AC sections)"
        )
    for marker in (
        "ac3296_1_typed_mid_ssot_first",
        "ac3296_2_steal_clear_join",
        "ac3296_3_soft_off_zero_cost",
        "ac3296_4_audit_join_source_cite",
        "ac3296_5_linter_and_no_invent",
    ):
        if marker not in test_src:
            fails.append(f"test_require_effect_auto_isolation.cpp: missing {marker} function")
    if "Issue #3296: typed-mid SSOT" not in test_src and "Issue #3296:" not in test_src:
        fails.append(
            "test_require_effect_auto_isolation.cpp: missing 'Issue #3296:' section "
            "header in run_test_require_effect_auto_isolation"
        )
    # AC5 forbids test_issue_3296.cpp (per #81967 + #1655).
    forbidden = REPO / "tests" / "compiler" / "test_issue_3296.cpp"
    if forbidden.exists():
        fails.append(
            f"{forbidden}: forbidden per #81967 / #1655 — extend test_require_effect_auto_isolation.cpp instead"
        )
    docs_design = REPO / "docs" / "design"
    if docs_design.is_dir():
        for entry in docs_design.iterdir():
            if entry.name.startswith("3296-"):
                fails.append(
                    f"{entry}: forbidden per #1655 — close comment carries design "
                    f"rationale, no docs/design/ for agent-developed repo"
                )
    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #3296 linter")
    parser.add_argument("--self-test", action="store_true", help="Self-check mode")
    parser.add_argument("--strict", action="store_true", help="Strict mode (exit 1 on any fail)")
    args = parser.parse_args()

    fails: list[str] = []

    esec = EVAL_SEC.read_text(encoding="utf-8")
    tma = TMA.read_text(encoding="utf-8")
    eval_ixx = EVAL_IXX.read_text(encoding="utf-8")
    test_src = TEST.read_text(encoding="utf-8")

    fails.extend(_check_require_effect_cascade(esec))
    fails.extend(_check_resolve_audit_mid(tma))
    fails.extend(_check_composite_batch_join_mid(tma))

    # evaluator.ixx:13063 quota use is allowed (mutation quota exceed log only).
    # We do NOT fail on it; instead record an info line if absent.
    if "process_resource_quota_manager().provenance_mutation_id" not in eval_ixx:
        fails.append(
            "evaluator.ixx: quota log path missing — diagnostic context for "
            "mutation quota exceed should still surface provenance_mutation_id "
            "(allowed exemption for #3296 AC1)"
        )

    fails.extend(_check_test_extensions(test_src))

    if args.self_test:
        # Sanity: this linter itself must be self-consistent.
        if not ISSUE_3296_ANCHOR.search(Path(__file__).read_text(encoding="utf-8")):
            fails.append("linter self-test: missing 'Issue #3296' anchor in own source")
        print("self-test OK" if not fails else "\n".join(fails))
        return 0 if not fails else 1

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed", file=sys.stderr)
        return 1 if args.strict else 1

    print("OK #3296 require_effect mid SSOT cascade: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
