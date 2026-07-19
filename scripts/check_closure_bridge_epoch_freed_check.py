#!/usr/bin/env python3
"""check_closure_bridge_epoch_freed_check.py — Issue #1656 source-level linter.

Verifies the 10 acceptance criteria for the freed-bitmap guard in
aura_get_closure_bridge_epoch / aura_get_closure_defuse_version.

The C++ runtime extern accessors in src/compiler/aura_jit_runtime.cpp
return per-closure provenance values. Before #1656, they did not consult
the g_closure_freed[cid] bitmap — freed slots returned stale values,
which would mislead emit-side LLVM IR freshness probes.

This linter enforces source-level invariants that guarantee:
  1. The freed-bitmap check is present in BOTH accessors (not just one)
  2. The check uses g_closure_freed (the canonical freed bitmap from #1361)
  3. The check returns 0 for freed slots (the canonical "stale" signal)
  4. The size guard runs BEFORE the freed-bitmap check (bounds safety)
  5. The inline read in aura_closure_call (L1051/1053) is unchanged —
     that path has additional invariants and is intentionally not patched
     per #1656 issue body's "separate concern" note.
  6. Both accessors share the same #if-issue-tagged comment for tracking.
  7. aura_free_closure sets the g_closure_freed[cid] flag (existing invariant).
  8. aura_alloc_closure does NOT pre-clear the freed flag (it's 0 by vector
     init, but verify the path doesn't set it to 1).
  9. The test_issue_1485.cpp extension includes AC11 (freed → 0) + AC12 (live → non-zero).
 10. No extern "C" wrapper for the freed-bitmap accessor — we use 0 sentinel
     instead (Option A from #1656 issue body).

Exit 0 = all 10 ACs satisfied, exit 1 = any failure (with diagnostic).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME_CPP = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
TEST_1485 = ROOT / "tests" / "test_issue_1485.cpp"

# AC1-AC4: both accessors have the freed-bitmap guard
# AC5: inline read at L1051/1053 unchanged
# AC6: shared #1656 comment tag
# AC7: aura_free_closure sets g_closure_freed[cid]
# AC8: aura_alloc_closure does NOT set g_closure_freed[cid] to 1
# AC9: test_issue_1485.cpp includes AC11 + AC12
# AC10: no extern "C" wrapper for a freed-bitmap accessor


def check_dual_freed_guard(runtime_src: str) -> list[str]:
    """AC1-AC4: both accessors must check g_closure_freed + return 0."""
    failures: list[str] = []

    # Extract each accessor body via function regex.
    bridge_match = re.search(
        r'extern\s+"C"\s+std::uint64_t\s+aura_get_closure_bridge_epoch\([^)]*\)\s*\{(.*?)\n\}',
        runtime_src,
        re.DOTALL,
    )
    defuse_match = re.search(
        r'extern\s+"C"\s+std::uint64_t\s+aura_get_closure_defuse_version\([^)]*\)\s*\{(.*?)\n\}',
        runtime_src,
        re.DOTALL,
    )
    if not bridge_match:
        failures.append("AC1: aura_get_closure_bridge_epoch accessor not found")
    if not defuse_match:
        failures.append("AC1: aura_get_closure_defuse_version accessor not found")
    if not bridge_match or not defuse_match:
        return failures

    bridge_body = bridge_match.group(1)
    defuse_body = defuse_match.group(1)

    # AC2: each body references g_closure_freed
    for name, body in [("bridge_epoch", bridge_body), ("defuse_version", defuse_body)]:
        if "g_closure_freed" not in body:
            failures.append(
                f"AC2: aura_get_closure_{name} body does NOT check g_closure_freed "
                "(freed-bitmap guard missing — #1656 fix not applied)"
            )

    # AC3: each body returns 0 for the freed path (look for `return 0;` near
    # the freed check). Use a permissive pattern that allows any whitespace.
    for name, body in [("bridge_epoch", bridge_body), ("defuse_version", defuse_body)]:
        # Find the freed-check block and look for a return 0 within it
        freed_block = re.search(
            r"if\s*\([^)]*g_closure_freed[^)]*\)[^{]*\{[^}]*\}",
            body,
            re.DOTALL,
        )
        if freed_block and "return 0;" not in freed_block.group(0):
            failures.append(
                f"AC3: aura_get_closure_{name} freed-check block does not return 0 "
                "(must return 0 for freed slots, not the stale provenance)"
            )

    # AC4: size guard runs BEFORE freed-bitmap check (bounds safety — without
    # the size guard, the freed check would index out of bounds).
    for name, body in [("bridge_epoch", bridge_body), ("defuse_version", defuse_body)]:
        size_pos = body.find(
            "g_closure_bridge_epochs.size()" if name == "bridge_epoch" else "g_closure_defuse_versions.size()"
        )
        freed_pos = body.find("g_closure_freed")
        if size_pos == -1 or freed_pos == -1:
            failures.append(f"AC4: aura_get_closure_{name} missing size guard or freed check")
            continue
        if size_pos > freed_pos:
            failures.append(
                f"AC4: aura_get_closure_{name} size guard runs AFTER freed check — "
                "freed check could OOB-index. Must check size first."
            )

    return failures


def check_inline_read_unchanged(runtime_src: str) -> list[str]:
    """AC5: inline read at aura_closure_call already covered by #1361.

    The #1656 issue body mentions "the pre-existing inline read has the same
    gap" — but that's stale information. Issue #1361 already shipped the
    freed-bitmap check inside aura_closure_call (originally around L888 in
    the line numbers the issue body referenced):
        // Issue #1361: refuse to call a freed closure (graceful, no UAF)
        if (cid < g_closure_freed.size() && g_closure_freed[cid] != 0) return 0;
    So there's NO separate concern to fix in #1656 for the inline read path.
    This AC verifies the #1361 fix is still present (regression guard) and
    documents the actual state for future maintainers.
    """
    failures: list[str] = []
    # Find the aura_closure_call function body. No `extern "C"` in source —
    # the extern wrapper is declared in runtime_shared.h and applied at link time.
    # Function signature: `int64_t aura_closure_call(int64_t, int64_t*, int64_t)`.
    call_match = re.search(
        r"(?:^|\n)\s*int64_t\s+aura_closure_call\s*\([^)]*\)\s*\{(.*?)\n\}",
        runtime_src,
        re.DOTALL,
    )
    if not call_match:
        failures.append("AC5: aura_closure_call function not found")
        return failures
    call_body = call_match.group(1)
    # The #1361 fix must still be present — verify the freed-bitmap check
    # exists in the call path (so the #1656 fix's gap doesn't re-appear).
    if "g_closure_freed" not in call_body:
        failures.append(
            "AC5: aura_closure_call no longer checks g_closure_freed — "
            "the #1361 fix was reverted. This is a regression of #1361, not "
            "#1656, but #1656's audit documents it as the existing invariant."
        )
    if "#1361" not in call_body:
        failures.append("AC5: aura_closure_call missing #1361 tag comment on the freed check")
    return failures


def check_shared_comment_tag(runtime_src: str) -> list[str]:
    """AC6: both accessors share the #1656 issue-tagged comment for tracking."""
    failures: list[str] = []
    bridge_match = re.search(
        r'extern\s+"C"\s+std::uint64_t\s+aura_get_closure_bridge_epoch[^}]*\}',
        runtime_src,
        re.DOTALL,
    )
    defuse_match = re.search(
        r'extern\s+"C"\s+std::uint64_t\s+aura_get_closure_defuse_version[^}]*\}',
        runtime_src,
        re.DOTALL,
    )
    if bridge_match and "Issue #1656" not in bridge_match.group(0):
        failures.append("AC6: aura_get_closure_bridge_epoch missing 'Issue #1656' tag comment")
    if defuse_match and "Issue #1656" not in defuse_match.group(0):
        failures.append("AC6: aura_get_closure_defuse_version missing 'Issue #1656' tag comment")
    return failures


def check_free_sets_freed_flag(runtime_src: str) -> list[str]:
    """AC7: aura_free_closure sets g_closure_freed[cid] = 1."""
    failures: list[str] = []
    free_match = re.search(
        r"void\s+aura_free_closure\s*\([^)]*\)\s*\{(.*?)\n\}",
        runtime_src,
        re.DOTALL,
    )
    if not free_match:
        failures.append("AC7: aura_free_closure function not found")
        return failures
    free_body = free_match.group(1)
    if not re.search(r"g_closure_freed\[cid\]\s*=\s*1", free_body):
        failures.append(
            "AC7: aura_free_closure does NOT set g_closure_freed[cid] = 1 — "
            "the freed-bitmap guard relies on this invariant (#1361)"
        )
    return failures


def check_alloc_doesnt_set_freed(runtime_src: str) -> list[str]:
    """AC8: aura_alloc_closure must NOT set g_closure_freed[cid] = 1.

    It's 0 by vector resize default (resize with 0), so a fresh slot has
    freed[cid] == 0 implicitly. But verify the alloc path doesn't
    explicitly set it to 1 (which would mark a live slot as freed).
    """
    failures: list[str] = []
    # No `extern "C"` in source — same link-time-wrapper pattern as
    # aura_closure_call. Signature: `int64_t aura_alloc_closure(int64_t)`.
    alloc_match = re.search(
        r"(?:^|\n)\s*int64_t\s+aura_alloc_closure\s*\([^)]*\)\s*\{(.*?)\n\}",
        runtime_src,
        re.DOTALL,
    )
    if not alloc_match:
        failures.append("AC8: aura_alloc_closure function not found")
        return failures
    alloc_body = alloc_match.group(1)
    if re.search(r"g_closure_freed\[cid\]\s*=\s*1", alloc_body):
        failures.append(
            "AC8: aura_alloc_closure sets g_closure_freed[cid] = 1 — "
            "freshly allocated slots must be marked as live (freed[cid] = 0)"
        )
    return failures


def check_test_1485_acs(test_src: str) -> list[str]:
    """AC9: tests/test_issue_1485.cpp includes AC11 (freed → 0) + AC12 (live → non-zero)."""
    failures: list[str] = []
    if "AC11" not in test_src:
        failures.append("AC9: test_issue_1485.cpp missing AC11 marker")
    if "AC12" not in test_src:
        failures.append("AC9: test_issue_1485.cpp missing AC12 marker")
    # AC11 specifically should mention freed + 0
    ac11_match = re.search(r"AC11[\s\S]{0,2000}?AC12", test_src)
    if ac11_match:
        block = ac11_match.group(0)
        if "aura_free_closure" not in block:
            failures.append("AC9: AC11 block does not call aura_free_closure")
        if "return 0" not in block and "returns 0" not in block:
            failures.append("AC9: AC11 block does not verify returns-0 contract")
    # AC12 should mention non-zero + live
    ac12_match = re.search(r"AC12[\s\S]{0,2000}?(?:\n\s*//\s*─|\n\s*std::println)", test_src)
    if ac12_match:
        block = ac12_match.group(0)
        if "non-zero" not in block and "!= 0" not in block:
            failures.append("AC9: AC12 block does not verify non-zero contract")
    return failures


def check_no_freed_extern_wrapper(runtime_src: str, bridge_hdr: str) -> list[str]:
    """AC10: no extern 'C' wrapper for a freed-bitmap accessor — Option A uses 0 sentinel.

    The issue body's Option B (expose aura_is_closure_freed extern) was rejected
    in favor of Option A (return 0 for freed, simpler contract, harder to misuse).
    Verify no extern "C" wrapper for the freed-bitmap accessor was added.
    """
    failures: list[str] = []
    # Look for any new extern "C" function related to closure_freed
    if re.search(r'extern\s+"C"\s+\w+\s+aura_(?:is_closure_freed|closure_is_freed)\b', runtime_src):
        failures.append(
            "AC10: extern 'C' wrapper for closure-freed accessor found — "
            "#1656 chose Option A (return 0 sentinel), not Option B (expose freed accessor)"
        )
    if re.search(r"aura_(?:is_closure_freed|closure_is_freed)", bridge_hdr):
        failures.append(
            "AC10: aura_jit_bridge.h declares closure-freed accessor — "
            "#1656 chose Option A (return 0 sentinel), not Option B"
        )
    return failures


def main() -> int:
    if not RUNTIME_CPP.exists():
        print(f"FAIL: {RUNTIME_CPP} not found", file=sys.stderr)
        return 1
    if not TEST_1485.exists():
        print(f"FAIL: {TEST_1485} not found", file=sys.stderr)
        return 1

    runtime_src = RUNTIME_CPP.read_text(encoding="utf-8")
    test_src = TEST_1485.read_text(encoding="utf-8")
    bridge_hdr = (ROOT / "src" / "compiler" / "aura_jit_bridge.h").read_text(encoding="utf-8")

    all_failures: list[str] = []
    all_failures.extend(check_dual_freed_guard(runtime_src))
    all_failures.extend(check_inline_read_unchanged(runtime_src))
    all_failures.extend(check_shared_comment_tag(runtime_src))
    all_failures.extend(check_free_sets_freed_flag(runtime_src))
    all_failures.extend(check_alloc_doesnt_set_freed(runtime_src))
    all_failures.extend(check_test_1485_acs(test_src))
    all_failures.extend(check_no_freed_extern_wrapper(runtime_src, bridge_hdr))

    if all_failures:
        print(f"FAIL: {len(all_failures)} AC(s) violated:", file=sys.stderr)
        for f in all_failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print("check_closure_bridge_epoch_freed_check: 10/10 ACs satisfied ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
