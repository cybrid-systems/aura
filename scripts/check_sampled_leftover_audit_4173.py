#!/usr/bin/env python3
# scripts/check_sampled_leftover_audit_4173.py -- Issue #4173 source-cite gate.
#
# AC1: should_audit's Sampled skip arm forces the audit under
#      production_defaults_active — the #3530 audit-skipped SE stays as
#      joinable observability, but a non-hit id can no longer return
#      false (Success trail / invariant suite cannot be skipped for
#      small non-linear non-match dirty scopes).
# AC2: Soft keeps the zero-cost skip — the skip arm still returns false
#      when production is off, and the #3530 soft comment stays intact.
# AC3: decide() mirrors the forced face — production_leftover_force folds
#      into would_audit, force_reason "production-sampled-leftover" is
#      wired, and the hard-gate line is NOT widened (no session/context
#      force change).
# AC4: Runtime ACs live in tests/compiler/test_typed_mutation_audit_decision.cpp
#      (leftover force, soft skip unchanged, decide parity); linter
#      registered in build.py + root_check_allowlist.txt; no
#      docs/design/4173-*, no tests/**/test_issue_4173.cpp.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HDR = "src/compiler/typed_mutation_audit.h"
TEST = "tests/compiler/test_typed_mutation_audit_decision.cpp"
BUILD = "build.py"
ALLOW = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_sampled_leftover_audit_4173"
CITE = "Issue #4173: production leftover must not under-sample the"
FORCE = "production_leftover_force"
REASON = "production-sampled-leftover"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(hdr: str, test: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — the skip arm forces the audit under production, SE retained.
    pos = hdr.find(CITE)
    if pos == -1:
        fails.append("AC1: #4173 skip-arm cite not found")
    else:
        win = hdr[pos : pos + 700]
        if "if (prod)" not in win:
            fails.append("AC1: production force arm missing in the skip branch")
        if "return true;" not in win:
            fails.append("AC1: forced audit return missing in the skip branch")
        if "emit_security_event_durable(" not in hdr[max(0, pos - 900) : pos]:
            fails.append("AC1: #3530 audit-skipped SE emit not retained ahead of the force")
    must('"audit-skipped"', "AC1 SE reason", hdr)
    must('"sampled-ratio-skip"', "AC1 SE op", hdr)
    must("Issue #4173: under production_defaults_active a Sampled skip is", "AC1 doc cite", hdr)

    # AC2 — Soft keeps the zero-cost skip.
    if pos != -1:
        tail = hdr[pos : pos + 900]
        if "return false;" not in tail:
            fails.append("AC2: soft skip return false missing after the force arm")
    must("Soft / !production: zero extra (one relaxed load, no emit).", "AC2 soft comment", hdr)

    # AC3 — decide() mirrors the forced face; hard gate NOT widened.
    must(
        f"const bool {FORCE} = d.production_defaults && d.sample_ratio > 1;",
        "AC3 force expr",
        hdr,
    )
    must(
        "context_force || sample_hit || mutate_session_force || production_leftover_force;",
        "AC3 would_audit fold",
        hdr,
    )
    must(f'd.force_reason = "{REASON}";', "AC3 force reason", hdr)
    must(
        "d.would_hard_gate = strict_sandbox || context_force || mutate_session_force;",
        "AC3 hard gate unchanged",
        hdr,
    )
    must('"production-sampled-', "AC3 decision-table row", hdr)

    # AC4 — runtime ACs + wiring + no invent.
    must("Issue #4173", "AC4 test cite", test)
    must("inject_sampled_ratio_for_test(4)", "AC4 leftover seam", test)
    must("4173 AC1: should_audit(1) true", "AC4 force AC", test)
    must("4173 AC2: soft non-hit still skipped", "AC4 soft AC", test)
    must(f'"{REASON}"', "AC4 reason pin", test)
    must(LINTER, "AC4 build wiring", build)
    must(LINTER, "AC4 allowlist", allow)
    if (ROOT / "tests/compiler/test_issue_4173.cpp").exists():
        fails.append("AC4: invented tests/compiler/test_issue_4173.cpp")
    if any(p.name.startswith("4173-") for p in (ROOT / "docs" / "design").glob("*")):
        fails.append("AC4: docs/design/4173-* exists (no design docs per #1655)")
    return fails


def _self_test() -> int:
    hdr = _read(HDR)
    test_ok = _read(TEST)
    build_ok = _read(BUILD)
    allow_ok = _read(ALLOW)
    bad = _rows(hdr, test_ok, build_ok, allow_ok)
    if bad:
        for r in bad:
            print(f"FAIL {LINTER} self-test baseline: {r}")
        return 1
    # Strip the force arm + decide mirror; the gate rows must trip.
    stripped = hdr.replace(CITE, "/* stripped */")
    stripped = stripped.replace(
        "        if (prod)\n            return true;\n        return false;", "        return false;"
    )
    stripped = stripped.replace(f"    const bool {FORCE} = d.production_defaults && d.sample_ratio > 1;\n", "")
    stripped = stripped.replace(" || production_leftover_force;", ";")
    stripped = stripped.replace(f'd.force_reason = "{REASON}";', "")
    test_bad = test_ok.replace("inject_sampled_ratio_for_test(4)", "/* stripped */")
    bad2 = _rows(stripped, test_bad, build_ok, allow_ok)
    if not any("force arm" in r or "forced audit" in r or "force expr" in r or "leftover seam" in r for r in bad2):
        print("self-test: stripped fixture did not trip the AC1/AC3 gate rows")
        return 1
    print(f"ok {LINTER} self-test")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #4173 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any contract row")
    ap.add_argument("--self-test", action="store_true", help="run synthetic fixtures")
    args = ap.parse_args()
    if args.self_test:
        return _self_test()
    rows = _rows(_read(HDR), _read(TEST), _read(BUILD), _read(ALLOW))
    if rows:
        for r in rows:
            print(f"FAIL {LINTER}: {r}")
        return 1
    print(f"ok {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
