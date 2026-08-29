#!/usr/bin/env python3
"""Issue #3406: outermost persist — recover-fail must clear persist buffer + bump mismatch.

Residual from #3376. The recover-fail branch in
`aura_outermost_success_persist_occurrence` (after
`!tc->ensure_occurrence_commit_or_recover()`) was the odd arm: drain,
fingerprint, mid-abort, pending-face, and ADT-exhaust reject paths all
called `clear_occurrence_persist_buffer(tc)` + bumped the mismatch
counter; recover-fail only stamped the proof + cleared authority.
Without the buffer clear, a concurrent densify/steal rehydrate copies
the just-written snapshot and a later outermost merge freezes the
narrowing that `ensure_*` already declared unrecoverable (I4
"过期窄化仍能用" at the persist/rehydrate face, not the query face).

Contract:
  AC1 The recover-fail branch in `aura_outermost_success_persist_occurrence`
     (`src/compiler/evaluator_mutation_boundary.cpp`) calls
     `(void)aura::compiler::typed_audit::clear_occurrence_persist_buffer(tc)`
     AFTER `clear_type_linear_commit_proof_on_abort()` and BEFORE the
     `return; // skip grant...` line.
  AC2 The same branch also calls
     `ev->bump_occurrence_persist_fingerprint_mismatch()` (matches
     the drain/fingerprint/ADT reject pattern).
  AC3 Source-cite anchor: the recover-fail branch comment cites #3406.
  AC4 Existing 5 reject paths (fingerprint mismatch, mid-abort,
     drain non-SOLVED, pending face hit, ADT exhaust) still call
     `clear_occurrence_persist_buffer(tc)` — count invariant
     `clear_occurrence_persist_buffer(tc)` >= 6 inside the helper
     (5 existing + 1 from #3406).
  AC5 No new query key, no new force_reason family. Reuse existing
     force_reason 16 + bump_occurrence_persist_fingerprint_mismatch
     counter.
  AC6 No `docs/design/3406-*` (per #1655); no
     `tests/compiler/test_issue_3406.cpp` (per #81934).
  AC7 Source-cite #3406 + build.py registration; no design docs.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _count(hay: str, needle: str) -> int:
    c = 0
    pos = 0
    while True:
        pos = hay.find(needle, pos)
        if pos == -1:
            return c
        c += 1
        pos += len(needle)


def main() -> int:
    fails: list[str] = []

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    build = _read("build.py")

    fn_pos = emb.find('extern "C" void aura_outermost_success_persist_occurrence(')
    if fn_pos == -1:
        fails.append(
            "AC1: aura_outermost_success_persist_occurrence helper not found "
            "in src/compiler/evaluator_mutation_boundary.cpp (#3406 contract "
            "anchor missing)"
        )
        emb_after = ""
    else:
        emb_after = emb[fn_pos:]

    recover_pos = emb_after.find("if (!tc->ensure_occurrence_commit_or_recover())")
    return_pos = -1
    if recover_pos != -1:
        return_pos = emb_after.find(
            "return; // skip grant; recover face stamps via publish_occurrence_commit_health",
            recover_pos,
        )

    # AC1: recover-fail branch calls clear_occurrence_persist_buffer(tc)
    # AFTER clear_type_linear_commit_proof_on_abort() and BEFORE the return.
    if recover_pos == -1:
        fails.append(
            "AC1: `if (!tc->ensure_occurrence_commit_or_recover())` branch "
            "not found in aura_outermost_success_persist_occurrence (#3376 "
            "recover-fail check regressed)"
        )
    elif return_pos == -1:
        fails.append("AC1: recover-fail `return; // skip grant...` line not found (branch shape changed unexpectedly)")
    else:
        branch = emb_after[recover_pos:return_pos]
        clear_buf_pos = branch.find("clear_occurrence_persist_buffer(tc)")
        clear_proof_pos = branch.find("clear_type_linear_commit_proof_on_abort()")
        if clear_proof_pos == -1:
            fails.append(
                "AC1: clear_type_linear_commit_proof_on_abort() missing "
                "from recover-fail branch (#3376 contract regressed)"
            )
        if clear_buf_pos == -1:
            fails.append(
                "AC1: clear_occurrence_persist_buffer(tc) MISSING from "
                "recover-fail branch — densify/steal rehydrate can freeze "
                "the rejected snapshot (#3406 contract not shipped)"
            )
        elif clear_proof_pos != -1 and clear_buf_pos <= clear_proof_pos:
            fails.append(
                "AC1: clear_occurrence_persist_buffer(tc) must come AFTER "
                "clear_type_linear_commit_proof_on_abort() in the "
                "recover-fail branch (ordering matches the other reject arms)"
            )

    # AC2: same branch bumps the mismatch counter (matches the other
    # reject arms).
    if recover_pos != -1 and return_pos != -1:
        branch = emb_after[recover_pos:return_pos]
        if "bump_occurrence_persist_fingerprint_mismatch" not in branch:
            fails.append(
                "AC2: recover-fail branch does not bump "
                "bump_occurrence_persist_fingerprint_mismatch — drain/"
                "fingerprint/ADT reject paths bump, recover-fail should "
                "match for counter parity"
            )

    # AC3: source-cite #3406 in the recover-fail branch comment.
    if recover_pos != -1 and return_pos != -1:
        branch = emb_after[recover_pos:return_pos]
        if "#3406" not in branch:
            fails.append(
                "AC3: recover-fail branch is missing the #3406 source-cite "
                "anchor (comment must reference #3406 to document the fix)"
            )

    # AC4: existing 5 reject paths still call clear_occurrence_persist_buffer;
    # total >= 6 inside the helper.
    if emb_after:
        total = _count(emb_after, "clear_occurrence_persist_buffer(tc)")
        if total < 6:
            fails.append(
                f"AC4: clear_occurrence_persist_buffer(tc) call count = "
                f"{total}, expected >= 6 (5 existing reject paths + "
                f"#3406 recover-fail). Recover-fail clear missing or "
                f"existing rejects regressed."
            )
        before_recover = emb_after[:recover_pos] if recover_pos != -1 else emb_after
        before_count = _count(before_recover, "clear_occurrence_persist_buffer(tc)")
        if before_count < 5:
            fails.append(
                f"AC4: existing 5 reject paths no longer all call "
                f"clear_occurrence_persist_buffer — before-recover-fail "
                f"count = {before_count}, expected >= 5 (#3376 contract "
                f"regressed)"
            )

    # AC5: no new query key, no new force_reason family.
    # The recover-fail branch must reuse force_reason 16 (existing family)
    # and must not introduce a new query key.
    if recover_pos != -1 and return_pos != -1:
        branch = emb_after[recover_pos:return_pos]
        # Check no new force_reason beyond 16.
        force_reasons = re.findall(r"force_reason\s*=\s*(\d+)", branch)
        non_standard = [fr for fr in force_reasons if fr != "16"]
        if non_standard:
            fails.append(
                f"AC5: recover-fail branch introduces new force_reason "
                f"value(s) {non_standard}; must reuse force_reason 16 "
                f"(abort-class family)"
            )
        # No new query primitive — the branch is an internal reject
        # helper; new query keys would be added via
        # evaluator_primitives_query.cpp, not here. Confirm by counting
        # query key strings — none expected in this branch.
        if "query:" in branch:
            fails.append(
                "AC5: recover-fail branch references `query:` key — new query key not allowed (must reuse existing)"
            )

    # AC6: no docs/design/3406-*.md, no tests/compiler/test_issue_3406.cpp.
    if list((ROOT / "docs" / "design").glob("3406-*.md")):
        fails.append("AC6: docs/design/3406-*.md exists — design docs banned per #1655")
    if (ROOT / "tests" / "compiler" / "test_issue_3406.cpp").is_file():
        fails.append(
            "AC6: tests/compiler/test_issue_3406.cpp exists — must extend "
            "existing test_outermost_persist_fail_closed.cpp per #81934"
        )

    # AC7: build.py registration.
    if "check_recover_fail_clear_persist_3406" not in build:
        fails.append(
            "AC7: build.py does not register "
            "check_recover_fail_clear_persist_3406 (linter not wired into "
            "the coverage gate)"
        )

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("PASS: #3406 recover-fail clear persist contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
