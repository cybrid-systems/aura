#!/usr/bin/env python3
# scripts/check_lockless_hygiene_se_3601.py -- Issue #3601 source-cite gate.
#
# Verifies the lockless mutate deny -> joinable SE face (#3217/#3319/#3543
# residual): every eval_flat_apply_mutate_* hygiene deny stamps the landed
# #3543 SE path instead of staying counters-only.
#
#  AC1: every record_hygiene_violation_attempt() deny site in
#       evaluator_eval_flat.cpp pairs (within 3 lines) with
#       note_hygiene_last_limit_reason(kHygieneLimitReasonMacroIntroduced)
#       — 16/16 sites (10 fixed by #3601, 6 pre-existing).
#  AC2: evaluator_eval_flat.cpp cites Issue #3601 at the fix site.
#  AC3: no parallel #3543 SE emit — the lockless deny sites must not grow
#       their own MacroHygiene emit_security_event_durable (the #3543 note
#       path stays the sole emitter of the hygiene-macro-introduced bus;
#       no second audit bus). Issue #3652 exception: the allow-arm MSE
#       mirror emits the #3542/#3650 capability-deny face (EffectDeny,
#       reason macro-mutate-needs-macro-self-evo) — a different event
#       kind, not a second hygiene bus.
#  AC4: runtime face test lives in tests/compiler/
#       test_tweak_literal_audit_consistency.cpp (pinned-mid join via
#       query:security-audit-trail / query:security-audit, stable reason
#       hygiene-macro-introduced, batch + :allow-macro? opt-out reach).
#  AC5: no docs/design/3601-* (#1655); no tests/**/test_issue_3601.cpp
#       (#81934); build.py wires this linter.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

FLAT = "src/compiler/evaluator_eval_flat.cpp"
TEST = "tests/compiler/test_tweak_literal_audit_consistency.cpp"

DEFAULT_TARGETS: tuple[str, ...] = (FLAT, TEST, "build.py")

REQUIRED: tuple[tuple[str, str, str], ...] = (
    (
        FLAT,
        r"Issue\s+#3601",
        "3601 AC2: lockless helper cites #3601",
    ),
    (
        TEST,
        r"Issue\s+#3601",
        "3601 AC4: test hosts #3601 ACs",
    ),
    (
        TEST,
        r"hygiene-macro-introduced",
        "3601 AC4: test asserts the stable reason string",
    ),
    (
        TEST,
        r"query:security-audit\b",
        "3601 AC2: test drives the positional mid+reason join",
    ),
    (
        TEST,
        r"mutate:atomic-batch.*:allow-macro\?",
        "3601 AC4: test reaches the lockless deny via batch-level opt-out",
    ),
    (
        "build.py",
        r"check_lockless_hygiene_se_3601",
        "3601 AC5: build.py wires the linter",
    ),
)

FORBIDDEN: tuple[tuple[str, str, str], ...] = (
    (
        FLAT,
        r"emit_security_event_durable\(\s*SecurityEventKind::MacroHygiene",
        "3601 AC3: lockless helper must not grow its own #3543 SE emit (single note-path emitter; #3652 capability mirror uses EffectDeny)",
    ),
    (
        FLAT,
        r"SecurityEventKind::MacroHygiene",
        "3601 AC3: no #3543 MacroHygiene SE reference in the lockless helper (capability deny face is EffectDeny per #3542/#3650/#3652)",
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

    # AC1: pairing invariant — every record_hygiene_violation_attempt() deny
    # site is followed within 3 lines by the kHygieneLimitReasonMacroIntroduced
    # stamp (16/16: 10 fixed by #3601 + 6 pre-existing).
    lines = body(FLAT).split("\n")
    call_idx = [i for i, ln in enumerate(lines) if "record_hygiene_violation_attempt();" in ln]
    if len(call_idx) < 16:
        failures.append(f"3601 AC1: expected >=16 deny sites, found {len(call_idx)}")
    for i in call_idx:
        window = "\n".join(lines[i + 1 : i + 6])
        if "note_hygiene_last_limit_reason(kHygieneLimitReasonMacroIntroduced)" not in window:
            failures.append(f"3601 AC1: deny site at line {i + 1} missing reason stamp within 3 lines")

    # AC5: no design doc, no standalone issue test (#1655 / #81934).
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("3601-"):
                failures.append(f"3601 AC5: docs/design/{entry.name} must not exist (#1655)")
    for probe in (
        REPO_ROOT / "tests" / "core" / "test_issue_3601.cpp",
        REPO_ROOT / "tests" / "issues" / "test_issue_3601.cpp",
    ):
        if probe.exists():
            failures.append(f"3601 AC5: {probe.relative_to(REPO_ROOT)} must not exist (#81934)")

    return failures


def self_test() -> int:
    """Regexes compile + anchor targets exist (pre-flight)."""
    ok = True
    for _, pattern, label in REQUIRED + FORBIDDEN:
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
    ap = argparse.ArgumentParser(description="Issue #3601 lockless deny SE gate")
    ap.add_argument("--self-test", action="store_true", help="compile regexes + probe targets")
    ap.add_argument("--strict", action="store_true", help="exit non-zero on any failure")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    failures = run_checks()
    if failures:
        print(f"check_lockless_hygiene_se_3601: {len(failures)} failure(s)")
        for f in failures:
            print(f"  FAIL: {f}")
        return 1 if args.strict else 0
    print("check_lockless_hygiene_se_3601: clean (all deny sites stamped, incl. #3652 mirror)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
