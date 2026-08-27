#!/usr/bin/env python3
# scripts/check_steal_abort_reason_coverage_3303.py — Issue #3303 source-cite gate.
#
# Verifies the steal-abort path in macro_expansion.cpp wires the stable
# agent-facing reason (kHygieneLimitReasonStealAbort = 6, not the old
# semantically-wrong PassLimit code 3):
#
#   AC1 — ixx declares kHygieneLimitReasonStealAbort = 6
#   AC2 — steal-abort site stamps kHygieneLimitReasonStealAbort via both
#         g_macro_clone_last_reject_reason.store(...) AND
#         note_hygiene_last_limit_reason(kHygieneLimitReasonStealAbort)
#   AC3 — hygiene_last_limit_reason_string() switch handles case 6 with
#         return "steal-abort"
#   AC4 — steal0 capture at function entry runs at ALL depths (was
#         hygiene_depth == 0 only)
#   AC5 — steal detection site at function exit runs at ALL depths
#         (outer guard is `if (new_id != NULL_NODE)`, not
#         `if (hygiene_depth == 0 && new_id != NULL_NODE)`)
#   AC6 — g_macro_clone_nested_steal_check_total counter exists, defined
#         in cpp, and bumped at depth>0
#   AC7 — ConcurrentCloneGuard ownership documentation comment block
#         mentions top-level / nested split
#   AC8 — aura_test_reset_macro_clone_same_flat_reject_for_test() also
#         resets g_macro_clone_nested_steal_check_total
#
# Default: --strict. CI gate.
#
# Self-test:
#   python3 scripts/check_steal_abort_reason_coverage_3303.py --self-test
#
# Catches regressions when a steal-abort path is added (e.g. another
# caller enters the steal detection site) but the StealAbort reason
# stamp is forgotten — would re-open the #3303 residual where agent
# replay sees the wrong reason string ("hygiene-pass-limit" instead of
# "steal-abort").

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/macro_expansion.cpp",
    "src/compiler/macro_expansion.ixx",
)


def _read(rel: str) -> str:
    p = REPO_ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _check_steal_abort_site(me: str) -> list[str]:
    """Verify the steal-abort site stamps kHygieneLimitReasonStealAbort
    (not the old PassLimit code 3) via both store + note_hygiene_last_limit_reason."""
    failures: list[str] = []
    pos = me.find("g_macro_clone_steal_abort_total.fetch_add")
    if pos < 0:
        failures.append("AC2: steal-abort anchor (g_macro_clone_steal_abort_total.fetch_add) not found")
        return failures
    scope = me[pos : pos + 1200]
    if "kHygieneLimitReasonStealAbort" not in scope:
        failures.append("AC2: steal-abort site does not use kHygieneLimitReasonStealAbort")
    if "note_hygiene_last_limit_reason(kHygieneLimitReasonStealAbort)" not in scope:
        failures.append("AC2: steal-abort site does not call note_hygiene_last_limit_reason(StealAbort)")
    if re.search(r"\.store\(\s*3\s*,", scope):
        failures.append("AC2: steal-abort site still uses store(3,...) — old PassLimit code, semantically wrong")
    return failures


def _check_steal0_capture(me: str) -> list[str]:
    """Verify steal0 capture at function entry runs at all depths
    (no hygiene_depth == 0 ternary gate)."""
    failures: list[str] = []
    m = re.search(r"const\s+auto\s+steal0\s*=", me)
    if not m:
        failures.append("AC4: steal0 capture line not found")
        return failures
    pos = m.start()
    scope = me[pos : pos + 600]
    if re.search(r"hygiene_depth\s*==\s*0\s*\?", scope):
        failures.append("AC4: steal0 capture still gated on hygiene_depth == 0")
    if "aura_fiber_static_cross_fiber_mutation_safe_steal_total()" not in scope:
        failures.append("AC4: steal0 capture missing fiber-steal-total call")
    return failures


def _check_detection_scope(me: str) -> list[str]:
    """Verify the steal detection site runs at all depths
    (outer guard is `if (new_id != NULL_NODE)`, not depth-gated)."""
    failures: list[str] = []
    pos = me.find("g_macro_clone_nested_steal_check_total.fetch_add")
    if pos < 0:
        failures.append("AC5/AC6: nested steal-check counter bump not found")
        return failures
    scope_start = pos - 400 if pos >= 400 else 0
    scope = me[scope_start : pos + 400]
    if "hygiene_depth == 0 && new_id != NULL_NODE" in scope:
        failures.append("AC5: steal detection still gated on hygiene_depth == 0")
    if "if (new_id != NULL_NODE)" not in scope:
        failures.append("AC5: outer guard should be `if (new_id != NULL_NODE)`")
    return failures


def _check_nested_counter_bump(me: str) -> list[str]:
    """Verify g_macro_clone_nested_steal_check_total counter exists
    and is bumped at depth>0. The `if (hygiene_depth > 0)` gate is on
    the line(s) immediately BEFORE the fetch_add call, so we look at
    200 chars before + 250 chars after to catch it."""
    failures: list[str] = []
    pos = me.find("g_macro_clone_nested_steal_check_total.fetch_add")
    if pos < 0:
        failures.append("AC6: nested steal-check counter bump not found")
        return failures
    scope_start = max(0, pos - 200)
    scope = me[scope_start : pos + 250]
    if "hygiene_depth > 0" not in scope:
        failures.append("AC6: nested counter bump not gated on hygiene_depth > 0")
    return failures


def _check_ownership_doc(me: str) -> list[str]:
    """Verify ConcurrentCloneGuard ownership documentation block."""
    failures: list[str] = []
    if "TOP-LEVEL OWNS name_map FOR THE WHOLE SUBTREE" not in me:
        failures.append("AC7: ownership contract (top-level owns name_map) not documented")
    if "NESTED NEVER RE-CLAIMS" not in me:
        failures.append("AC7: nested never re-claims contract not documented")
    return failures


def _check_reset_function(me: str) -> list[str]:
    """Verify the C bridge reset function clears the new counter."""
    failures: list[str] = []
    pos = me.find("aura_test_reset_macro_clone_same_flat_reject_for_test")
    if pos < 0:
        failures.append("AC8: C bridge reset function not found")
        return failures
    scope = me[pos : pos + 600]
    if "g_macro_clone_nested_steal_check_total.store(0," not in scope:
        failures.append("AC8: C bridge reset does not clear g_macro_clone_nested_steal_check_total")
    return failures


def _check_ixx_constants(ixx: str) -> list[str]:
    """Verify ixx declares kHygieneLimitReasonStealAbort = 6."""
    failures: list[str] = []
    if "kHygieneLimitReasonStealAbort = 6" not in ixx:
        failures.append("AC1: kHygieneLimitReasonStealAbort = 6 not declared in ixx")
    if "g_macro_clone_nested_steal_check_total" not in ixx:
        failures.append("AC1: g_macro_clone_nested_steal_check_total not declared in ixx")
    return failures


def _check_string_function(me: str) -> list[str]:
    """Verify hygiene_last_limit_reason_string() returns 'steal-abort' for code 6."""
    failures: list[str] = []
    m = re.search(r"hygiene_last_limit_reason_string\(\)[^{]*\{([^}]+case 6:[^}]+)\}", me, re.DOTALL)
    if not m:
        # Fallback: find case 6 and the next 200 chars.
        pos = me.find("case 6:")
        if pos < 0:
            failures.append("AC3: case 6 missing from hygiene_last_limit_reason_string switch")
            return failures
        scope = me[pos : pos + 200]
    else:
        scope = m.group(1)
    if 'return "steal-abort"' not in scope:
        failures.append('AC3: case 6 does not return "steal-abort"')
    return failures


def run_strict() -> list[str]:
    me = _read("src/compiler/macro_expansion.cpp")
    ixx = _read("src/compiler/macro_expansion.ixx")
    failures: list[str] = []
    failures.extend(_check_ixx_constants(ixx))
    failures.extend(_check_steal_abort_site(me))
    failures.extend(_check_string_function(me))
    failures.extend(_check_steal0_capture(me))
    failures.extend(_check_detection_scope(me))
    failures.extend(_check_nested_counter_bump(me))
    failures.extend(_check_ownership_doc(me))
    failures.extend(_check_reset_function(me))
    return failures


def _self_test() -> int:
    # Run the linter against the current repo; expect zero failures.
    failures = run_strict()
    if failures:
        print("SELF-TEST FAIL:", file=sys.stderr)
        for f in failures:
            print("  -", f, file=sys.stderr)
        return 1
    print("SELF-TEST OK: all #3303 source-cite checks pass")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1] if __doc__ else "")
    ap.add_argument(
        "--self-test", action="store_true", help="Run the linter against the current repo; expect zero failures."
    )
    ap.add_argument(
        "--strict", action="store_true", default=True, help="Default mode: emit failures and exit non-zero on any."
    )
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures = run_strict()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: #3303 source-cite checks pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
