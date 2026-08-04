#!/usr/bin/env python3
"""Issue #2593: forbid advertising parallel-intend :pure #t as transactional isolation.

Wording-drift gate: scans Agent-facing orch / parallel-intend surfaces
(src/orch/, src/compiler/evaluator_primitives_agent.cpp, docs/) for
phrases that pair pure-parallel terminology with transactional / ACID /
serializable isolation-level claims BEYOND `best-effort-pure`.

Allowed: disclaimer lines containing `never` / `NOT` / `not` /
`best-effort` / `forbid` / `footgun` / `must not` / `do not` / `cannot`
/ `isn't` / `must never` / `avoid` / `prohibit` / `caveat` / `warning` etc.

Banned patterns:
  - `pure` (or pure-mode / :pure / parallel-intend / isolation-level) within
    a line that also contains `transactional` / `ACID` / `serializable`,
    WITHOUT a disclaimer keyword on the same line.
  - `isolation-level = "transactional"` or
    `isolation-level = "serializable"` (hard value claim).

Exit 0 = clean; non-zero = banned wording detected.

Self-test mode (`--self-test`) verifies the gate correctly flags injected
forbidden wording and accepts allowed disclaimer wording.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Scan targets: orch README + agent primitives + key docs. Pure README
# (src/orch/README.md) carries the canonical pitfalls + closing line.
# The agent primitive file holds the batch hash construction.
SCAN_TARGETS = [
    "src/orch/README.md",
    "src/compiler/evaluator_primitives_agent.cpp",
    "docs/agent-orchestration-status.md",
]

# Files we never scan (auto-generated, contains identifiers without
# natural-language context).
SKIP_PATHS = {
    "docs/generated/test-registry.json",
}

# Disclaimer keywords (case-insensitive). A line that contains ANY of
# these markers is exempt from the wording-drift gate (the line is
# explicitly forbidding / disclaiming transactional advertising).
DISCLAIMER_RE = re.compile(
    r"\b(?:"
    r"never|NOT|not\b|best-effort|forbid(?:den)?|footgun|"
    r"isn't|aren't|don't|won't|can't|mustn't|shouldn't|"
    r"is\s+not|are\s+not|must\s+not|should\s+not|do\s+not|"
    r"must\s+never|never\s+advertise|not\s+advertise|"
    r"no\s+transactional|avoid|prohibit(?:ed|s)?|caveat|warn(?:ing)?|"
    r"do\s+not\s+advertise|advertise\b"
    r")\b",
    re.IGNORECASE,
)

# Banned pure + transactional/ACID/serializable co-occurrence (both orders).
# Trigger keywords are PURE terminology only — the default `parallel-intend`
# path is genuinely more serialized and may legitimately use "transactional"
# to describe its eval_mu mutex. Drift to forbid is the PURE path being
# advertised as transactional / ACID / serializable isolation.
PURE_TX_PAIR = re.compile(
    r"\b(?:pure|"
    r"pure-mode|pure_mode|"
    r"pure-path|pure_path|"
    r"pure-parallel|pure_parallel|"
    r"pure-contract|pure_contract|"
    r"pure-apply|pure_apply|"
    r"pure-applies|pure_applies|"
    r"pure-unlocked|pure_unlocked|"
    r"pure-fallback|pure_fallback|"
    r"pure-violation|pure_violation|"
    r"pure-batch|pure_batch)\b"
    r"[^\n]*?"
    r"\b(?:transactional|ACID|serializable)\b",
    re.IGNORECASE,
)
TX_PURE_PAIR = re.compile(
    r"\b(?:transactional|ACID|serializable)\b"
    r"[^\n]*?"
    r"\b(?:pure|"
    r"pure-mode|pure_mode|"
    r"pure-path|pure_path|"
    r"pure-parallel|pure_parallel|"
    r"pure-contract|pure_contract|"
    r"pure-apply|pure_apply|"
    r"pure-applies|pure_applies|"
    r"pure-unlocked|pure_unlocked|"
    r"pure-fallback|pure_fallback|"
    r"pure-violation|pure_violation|"
    r"pure-batch|pure_batch)\b",
    re.IGNORECASE,
)
# Hard value claim: isolation-level = "transactional" / "serializable".
ISO_LEVEL_CLAIM = re.compile(
    r"isolation[-_]level\s*[:=]\s*[\"']?(?:transactional|serializable)\b",
    re.IGNORECASE,
)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file() or rel in SKIP_PATHS:
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _scan_text(rel: str, text: str) -> list[str]:
    """Return a list of `rel:lineno: reason: <line>` violation strings."""
    violations: list[str] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not s:
            continue
        # Determine if any banned pattern matches this line.
        reason = None
        if ISO_LEVEL_CLAIM.search(line):
            reason = "isolation-level claims transactional/serializable"
        elif PURE_TX_PAIR.search(line) or TX_PURE_PAIR.search(line):
            reason = "pure paired with transactional/ACID/serializable"
        if reason is None:
            continue
        # Has banned pattern; check disclaimer.
        if DISCLAIMER_RE.search(line):
            continue
        violations.append(f"{rel}:{lineno}: {reason}: {s[:200]}")
    return violations


def _self_test() -> int:
    cases = [
        # (text, should_flag, description)
        # BAD: pure + transactional without disclaimer → flag
        (
            "The :pure #t path provides transactional isolation guarantees for Agent batches.",
            True,
            "pure + transactional no disclaimer",
        ),
        # BAD: pure + ACID without disclaimer → flag
        (
            "Pure mode provides ACID guarantees via the parallel_intend path.",
            True,
            "pure + ACID no disclaimer",
        ),
        # BAD: pure + serializable without disclaimer → flag
        (
            "Pure batches run with serializable isolation semantics.",
            True,
            "pure + serializable no disclaimer",
        ),
        # BAD: isolation-level = "transactional" hard value claim → flag
        (
            'isolation_level = "transactional"',
            True,
            "isolation-level value = transactional",
        ),
        # BAD: isolation-level : serializable → flag
        (
            "isolation-level: serializable  # pragma: serializable",
            True,
            "isolation-level value = serializable",
        ),
        # GOOD: explicit `Do NOT advertise` disclaimer → pass
        (
            "Do NOT advertise pure as transactional isolation (AC4 #2400 / #2230).",
            False,
            "Do NOT disclaimer",
        ),
        # GOOD: README pitfalls line with `not a transactional` → pass
        (
            "Pure is not a transactional isolation level; it is a best-effort probe.",
            False,
            "not a transactional + best-effort disclaimer",
        ),
        # GOOD: README closing line with `not advertise` → pass
        (
            "Do not advertise `:pure #t` as a transactional isolation level in any",
            False,
            "not advertise disclaimer",
        ),
        # GOOD: footnote `never transactional` → pass
        (
            "pure_mode=true → best-effort-pure (never transactional; even if all fallback)",
            False,
            "never transactional + best-effort disclaimer",
        ),
        # GOOD: footgun warning → pass
        (
            "Footgun: ACID/transactional wording in pure batches is a contract violation.",
            False,
            "footgun disclaimer (ACID/transactional in disclaimer context)",
        ),
        # GOOD: pure alone (no transactional/ACID/serializable) → pass
        (
            "The :pure #t contract unlocks concurrent apply for pure thunks.",
            False,
            "pure only, no transactional",
        ),
        # GOOD: transactional alone (no pure) → pass (not in Agent-facing pure surface)
        (
            "The eval_mu is a transactional lock for default parallel-intend.",
            False,
            "transactional alone (no pure keyword)",
        ),
    ]
    fails = 0
    for text, should_flag, desc in cases:
        violations = _scan_text("<self-test>", text)
        actually_flags = len(violations) > 0
        ok = actually_flags == should_flag
        marker = "OK  " if ok else "FAIL"
        print(f"{marker} [self-test] {desc}: expected flag={should_flag} got flag={actually_flags}")
        if not ok:
            for v in violations:
                print(f"    violation: {v}")
            fails += 1
    if fails:
        print(f"\n{fails} self-test case(s) failed", file=sys.stderr)
        return 1
    print("\nAll self-test cases pass (forbidden wording flagged, disclaimer lines accepted)")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return _self_test()

    # Positional argv entries are additional scan targets (relative to
    # repo root). Used by tests to inject drift into a tmp file and
    # verify the gate catches it end-to-end.
    extra_targets = [a for a in argv if not a.startswith("--")]

    violations: list[str] = []
    for rel in list(SCAN_TARGETS) + extra_targets:
        text = _read(rel)
        if not text:
            continue
        violations.extend(_scan_text(rel, text))

    if violations:
        for v in violations:
            print(f"FAIL: {v}", file=sys.stderr)
        print(
            f"\n{len(violations)} wording-drift row(s) detected. "
            f"Pure :pure #t must NEVER be advertised as transactional / ACID / "
            f"serializable isolation in Agent-facing schema text. Use a "
            f"`never` / `not` / `best-effort-pure` / `Do NOT advertise` "
            f"disclaimer, or reword.",
            file=sys.stderr,
        )
        return 1
    print("OK: Issue #2593 pure-parallel isolation wording — clean (no drift)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
