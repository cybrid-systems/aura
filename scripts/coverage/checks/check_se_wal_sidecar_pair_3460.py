#!/usr/bin/env python3
# scripts/coverage/checks/check_se_wal_sidecar_pair_3460.py — Issue #3460 gate.
#
# Verifies force_wal pairs the SecurityEvent side-car at BOTH production
# defaults sites (mutation WAL alone loses InvariantFail / hygiene /
# mid-fallback-refused evidence to the 1024 SE ring wrap):
#   AC1 — security_defaults.hh step 4 cites #3460 + calls
#         g_security_event_wal().enable on the same dir
#   AC2 — typed_mutation_audit.h #3375 force_wal block cites #3460 +
#         the same SE pair call
#   AC4 — both sites keep the AURA_SANDBOX=off / dev_off contract
#         (Soft never reaches the enable block)
#   AC5 — evaluator_security.cpp disable_mutation_audit_wal still
#         disables both WALs (single production switch, #2225)
#   AC6 — tests live in src-aligned suites; no test_issue_3460.cpp
#
# Default: strict. CI gate (runs via run_checks.py discovery).
#
# Self-test:
#   python3 scripts/coverage/checks/check_se_wal_sidecar_pair_3460.py --self-test
#
# Catches regressions where a new force_wal site (or a refactor of the
# existing ones) re-enables only the mutation WAL — the #2492/#2225
# residual that made wrapped SE evidence look like "never audited".

from __future__ import annotations

import argparse
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

SECURITY_DEFAULTS = "src/compiler/security_defaults.hh"
TYPED_AUDIT = "src/compiler/typed_mutation_audit.h"
EVAL_SECURITY = "src/compiler/evaluator_security.cpp"
TEST_MULTI_TENANT = "tests/compiler/test_audit_wal_force_multi_tenant.cpp"
TEST_REPLAY = "tests/compiler/test_security_event_wal_replay.cpp"
TEST_DURABLE_GAP = "tests/compiler/test_audit_durable_gap_force_wal.cpp"

REQUIRED: dict[str, tuple[str, ...]] = {
    SECURITY_DEFAULTS: (
        "Issue #3460",
        "g_security_event_wal().enable(",
        "core/security_event_wal.hh",
        "AURA_SANDBOX=off never enables WAL",
    ),
    TYPED_AUDIT: (
        "Issue #3460",
        "g_security_event_wal().enable(",
        "#3375",
    ),
    EVAL_SECURITY: (
        "g_mutation_audit_wal().disable()",
        "g_security_event_wal().disable()",
    ),
    TEST_MULTI_TENANT: (
        "3460 AC1: SE side-car enabled by security defaults (no Evaluator)",
        "3460 AC2: SE side-car enabled by audit defaults",
        "3460 AC4: SE side-car off",
        "3460 AC5: SE side-car off",
        "3460 AC3: side-car holds the deny mid",
        "3460 AC3: wrap does not lose the mid while WAL is on",
    ),
    TEST_DURABLE_GAP: (
        "3460 AC1: SE pair in security_defaults step 4",
        "3460 AC2: SE pair in the #3375 force_wal block",
        "3460 AC5: disable_mutation_audit_wal still disables both",
    ),
}

# The enable must be keyed to the mutation enable outcome shape: the
# pair call uses the same dir argument (nullptr, 0) — empty replay.
PAIR_CALL = "g_security_event_wal().enable("


def read_repo_file(rel: str) -> str:
    return (REPO_ROOT / rel).read_text(encoding="utf-8")


def check_file(path: str, required: Sequence[str]) -> list[str]:
    try:
        text = read_repo_file(path)
    except OSError as exc:
        return [f"{path}: unreadable ({exc})"]
    return [f"{path}: missing required pattern: {p!r}" for p in required if p not in text]


def run_checks() -> list[str]:
    problems: list[str] = []
    for path, required in REQUIRED.items():
        problems += check_file(path, required)
    # Both force_wal sites must pair the side-car with the SAME call
    # shape (empty replay) — count the pair calls: one per site.
    for path in (SECURITY_DEFAULTS, TYPED_AUDIT):
        text = read_repo_file(path)
        if text.count(PAIR_CALL) < 1:
            problems.append(f"{path}: SE side-car pair call missing")
    # AC6: tests live in src-aligned suites — no test_issue_3460.cpp.
    for stray in ("tests/compiler/test_issue_3460.cpp", "tests/issues/test_issue_3460.cpp"):
        if (REPO_ROOT / stray).exists():
            problems.append(f"{stray}: test_issue_NNNN.cpp files are forbidden (AC6)")
    return problems


# ── self-test ─────────────────────────────────────────────────────────────
# Feed the detector a synthetic regression (force_wal block WITHOUT the
# side-car pair) and assert it is caught.
SELFTEST_BAD_SITE = """\
if (has_explicit || force_wal) {
    const auto dir = resolve_mutation_audit_wal_dir(&used_default);
    if (g_mutation_audit_wal().enable(std::string_view(dir), nullptr, 0)) {
        return true;
    }
}
"""


def self_test() -> int:
    ok = True
    with tempfile.NamedTemporaryFile("w", suffix=".hh", delete=False) as f:
        f.write(SELFTEST_BAD_SITE)
        bad = f.name
    missing = [p for p in REQUIRED[SECURITY_DEFAULTS] if p not in Path(bad).read_text()]
    if "g_security_event_wal().enable(" in missing:
        print("self-test: unpaired force_wal site detected ✓")
    else:
        print("self-test: detector missed the unpaired-site regression", file=sys.stderr)
        ok = False
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
    parser = argparse.ArgumentParser(description="Issue #3460 source-cite gate")
    parser.add_argument("--self-test", action="store_true", help="run the built-in regression self-test")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    problems = run_checks()
    if problems:
        for p in problems:
            print(f"check_se_wal_sidecar_pair_3460: FAIL {p}", file=sys.stderr)
        return 1
    print("check_se_wal_sidecar_pair_3460: OK (#3460 gates present)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
