#!/usr/bin/env python3
"""Issue #4149 source-cite gate: MSE allow-arm deny stamps capability-deny (7).

The public deny_macro_opt_out_without_mse (evaluator_primitives_mutate.cpp)
emitted durable SE + record_hygiene_violation_attempt + typed audit but never
stamped note_hygiene_last_limit_reason — public mutate:* rejects for "needs
MacroSelfEvo" lacked the unified Agent last-limit surface that the lockless
twin and structural MacroIntroduced default-deny publish. The lockless twin
stamped kHygieneLimitReasonMacroIntroduced (4) for a capability miss, and the
public mutate:atomic-batch walker re-stamped 4 after the deny — agents could
not cleanly branch "unmarked mutate" (4) vs "need MacroSelfEvo".

ACs:
  AC1  the public deny helper stamps note_capability_deny_last_reason(
       kCapabilityDenyReasonNotGranted) — the #3304 companion API — inside
       the deny body and cites #4149; the capability sentinel stores 7 into
       the unified process-global hygiene atomic so
       hygiene_last_limit_reason_string() reads "capability-deny"; SE reason
       macro-mutate-needs-macro-self-evo stays. The per-fiber slot is
       deliberately NOT armed (fiber-slot 7 is the #4034/#4078
       expand-refuse arm — control-proven to refuse unrelated subsequent
       evals when stamped from a mutate context).
  AC2  the lockless deny (evaluator_eval_flat.cpp) stamps the same
       #3304 companion API and no longer stamps
       kHygieneLimitReasonMacroIntroduced inside the deny body; cites #4149.
  AC3  the public mutate:atomic-batch walker deny branch no longer stamps a
       reason after deny_macro_opt_out_without_mse (the helper owns the
       stamp; a caller-side 4 would clobber the LWW sentinel).
  AC4  the taxonomy stays: ceiling codes (1/2/3) cannot clobber a stored 4/5
       in note_hygiene_last_limit_reason_for_fiber (#3888 guard) and
       hygiene_limit_reason_string_for case 7 returns "capability-deny".
  AC5  tests cite #4149 (runtime ACs in test_hygiene_mutate_closed_loop);
       no tests/**/test_issue_4149.cpp; no docs/design/4149-*;
       build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MUT = ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
EFL = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
MCX = ROOT / "src" / "compiler" / "macro_expansion.cpp"
TST = ROOT / "tests" / "compiler" / "test_hygiene_mutate_closed_loop.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def _slice(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i)
    return text[i:j] if j > i else ""


def main() -> int:
    fails: list[str] = []

    mut = MUT.read_text()
    # AC1: public deny stamps the capability-deny sentinel.
    pub = _slice(
        mut,
        "deny_macro_opt_out_without_mse(Evaluator& ev",
        'return mev("hygiene-protected"',
    )
    if "note_capability_deny_last_reason(" not in pub:
        fails.append("AC1: public deny helper does not stamp note_capability_deny_last_reason")
    if "kCapabilityDenyReasonNotGranted" not in pub:
        fails.append("AC1: public deny helper does not stamp the not-granted capability reason")
    if "note_hygiene_last_limit_reason(" in pub:
        fails.append("AC1: public deny helper must not arm the per-fiber #4034/#4078 slot")
    if "#4149" not in pub:
        fails.append("AC1: public deny stamp does not cite #4149")
    if "macro-mutate-needs-macro-self-evo" not in pub:
        fails.append("AC1: public deny SE reason string changed")

    efl = EFL.read_text()
    lock = _slice(
        efl,
        "static bool deny_macro_opt_out_without_mse(Evaluator& ev",
        "return true;",
    )
    if "note_capability_deny_last_reason(" not in lock:
        fails.append("AC2: lockless deny does not stamp note_capability_deny_last_reason")
    if "kCapabilityDenyReasonNotGranted" not in lock:
        fails.append("AC2: lockless deny does not stamp the not-granted capability reason")
    if "kHygieneLimitReasonMacroIntroduced" in lock:
        fails.append("AC2: lockless deny still stamps macro-introduced (4) for a capability miss")
    if "note_hygiene_last_limit_reason(" in lock:
        fails.append("AC2: lockless deny must not arm the per-fiber #4034/#4078 slot")
    if "#4149" not in lock:
        fails.append("AC2: lockless deny stamp does not cite #4149")

    # AC3: the walker deny branch defers to the helper stamp.
    walk = _slice(
        mut,
        "if (deny_macro_opt_out_without_mse(ev, node, mev)) {",
        "bump_atomic_batch_hygiene_violation();",
    )
    if "note_hygiene_last_limit_reason" in walk:
        fails.append("AC3: batch walker deny branch re-stamps a reason (clobbers the helper's 7)")

    # AC3b: the batch sub-op conversion preserves a MacroSelfEvo-capability
    # diagnostic's capability-deny stamp (7) — only the naked structural
    # diagnostic stamps macro-introduced (4).
    conv = _slice(
        mut,
        'if (diag.message.find("MacroIntroduced") != std::string::npos) {',
        "bump_atomic_batch_hygiene_violation();",
    )
    if 'diag.message.find("MacroSelfEvo") == std::string::npos' not in conv:
        fails.append("AC3b: conversion does not preserve the capability-deny stamp for MacroSelfEvo diagnostics")
    if "Issue #4149" not in conv:
        fails.append("AC3b: conversion preserve branch does not cite #4149")

    # AC4: taxonomy anchors stay put.
    mcx = MCX.read_text()
    if "if (cur == kHygieneLimitReasonMacroIntroduced || cur == kHygieneLimitReasonRestUnmarked)" not in mcx:
        fails.append("AC4: ceiling LWW guard (#3888) missing in note_hygiene_last_limit_reason_for_fiber")
    if 'return "capability-deny";' not in mcx:
        fails.append("AC4: hygiene_limit_reason_string_for case 7 mapping missing")

    # AC5: tests + wiring + no artifacts.
    tst = TST.read_text()
    if tst.count("#4149") < 5:
        fails.append("AC5: test file does not cite #4149 (runtime ACs missing)")
    if (ROOT / "tests" / "compiler" / "test_issue_4149.cpp").exists():
        fails.append("AC5: tests/compiler/test_issue_4149.cpp must not exist (#81934)")
    if any((ROOT / "docs" / "design").glob("4149-*")):
        fails.append("AC5: docs/design/4149-* must not exist (#1655)")
    if "check_hygiene_limit_stamp_4149" not in BUILD.read_text():
        fails.append("AC5: build.py does not wire check_hygiene_limit_stamp_4149.py")
    if "check_hygiene_limit_stamp_4149.py" not in ALLOW.read_text():
        fails.append("AC5: scripts/coverage/root_check_allowlist.txt does not list the linter")

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("ok: #4149 hygiene limit stamp gate (5 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
