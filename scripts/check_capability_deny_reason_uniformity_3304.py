#!/usr/bin/env python3
# scripts/check_capability_deny_reason_uniformity_3304.py — Issue #3304 source-cite gate.
#
# Verifies that all production hygiene + MacroSelfEvo deny paths stamp a
# stable agent-facing reason (parallel structured code for capability —
# MacroSelfEvo sits outside the hygiene taxonomy). Catches regressions
# when a deny site is added (e.g. a new capability gate) but the parallel
# stable-code stamp is forgotten, which would re-open the #3304 residual
# (Agent tooling unable to distinguish "hygiene-macro-introduced" vs
# "gensym-ceiling" vs "depth-limit" vs "rest-unmarked" vs "steal-abort"
# vs MacroSelfEvo denial when only counters move).
#
# Contract rows (AC1–AC6 from the test file):
#
#   AC1 — macro_expansion.ixx declares kHygieneLimitReasonCapabilityDeny = 7
#   AC2 — macro_expansion.cpp case 7 → "capability-deny" in
#         hygiene_last_limit_reason_string
#   AC3 — capability_model.hh declares kCapabilityDenyReason* family + atomic
#         + note_capability_deny_last_reason + capability_deny_last_reason_string
#   AC4 — capability_model.hh check_macro_self_evo calls
#         note_capability_deny_last_reason(kCapabilityDenyReason*) at all
#         4 deny sites (NotGranted, ProvenanceFence, PolicyMissing, LimitsZero)
#   AC5 — note_capability_deny_last_reason also stamps
#         g_macro_hygiene_last_limit_reason(7) so the unified last_limit_reason
#         surface routes to capability_deny_last_reason_string()
#   AC6 — no remaining direct g_macro_hygiene_last_limit_reason.store(2,...)
#         or store(3,...) sites (refactored to note_hygiene_last_limit_reason)
#
# Default: --strict. CI gate.
#
# Self-test:
#   python3 scripts/check_capability_deny_reason_uniformity_3304.py --self-test

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/macro_expansion.cpp",
    "src/compiler/macro_expansion.ixx",
    "src/core/capability_model.hh",
)


def _read(rel: str) -> str:
    p = REPO_ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _check_ixx_sentinel(ixx: str) -> list[str]:
    """AC1: ixx declares kHygieneLimitReasonCapabilityDeny = 7."""
    failures: list[str] = []
    if "kHygieneLimitReasonCapabilityDeny = 7" not in ixx:
        failures.append("AC1: kHygieneLimitReasonCapabilityDeny = 7 not declared in ixx")
    return failures


def _check_case_seven(me: str) -> list[str]:
    """AC2: hygiene_last_limit_reason_string returns 'capability-deny' for code 7."""
    failures: list[str] = []
    pos = me.find("case 7:")
    if pos < 0:
        failures.append("AC2: case 7 missing from hygiene_last_limit_reason_string")
        return failures
    scope = me[pos : pos + 200]
    if 'return "capability-deny"' not in scope:
        failures.append('AC2: case 7 does not return "capability-deny"')
    return failures


def _check_capability_family(cap: str) -> list[str]:
    """AC3: kCapabilityDenyReason* family + accessors declared."""
    failures: list[str] = []
    required = (
        ("kCapabilityDenyReasonNone = 0", "AC3: kCapabilityDenyReasonNone = 0"),
        ("kCapabilityDenyReasonNotGranted = 1", "AC3: kCapabilityDenyReasonNotGranted = 1"),
        ("kCapabilityDenyReasonProvenanceFence = 2", "AC3: kCapabilityDenyReasonProvenanceFence = 2"),
        ("kCapabilityDenyReasonPolicyMissing = 3", "AC3: kCapabilityDenyReasonPolicyMissing = 3"),
        ("kCapabilityDenyReasonLimitsZero = 4", "AC3: kCapabilityDenyReasonLimitsZero = 4"),
        ("g_capability_deny_last_reason", "AC3: g_capability_deny_last_reason atomic"),
        ("note_capability_deny_last_reason", "AC3: note_capability_deny_last_reason"),
        ("capability_deny_last_reason_string", "AC3: capability_deny_last_reason_string"),
    )
    for needle, msg in required:
        if needle not in cap:
            failures.append(msg)
    # Stable strings in switch.
    strings = (
        ('"capability-not-granted"', "AC3: case 1 string"),
        ('"capability-provenance-fence"', "AC3: case 2 string"),
        ('"capability-policy-missing"', "AC3: case 3 string"),
        ('"capability-limits-zero"', "AC3: case 4 string"),
    )
    for needle, msg in strings:
        if needle not in cap:
            failures.append(msg)
    return failures


def _check_deny_sites(cap: str) -> list[str]:
    """AC4: 4 deny sites stamp new code; AC5: also stamps unified sentinel."""
    failures: list[str] = []
    sites = (
        (
            "note_capability_deny_last_reason(kCapabilityDenyReasonNotGranted)",
            "AC4: not-granted site stamps NotGranted",
        ),
        (
            "note_capability_deny_last_reason(kCapabilityDenyReasonProvenanceFence)",
            "AC4: provenance-fence site stamps ProvenanceFence",
        ),
        (
            "note_capability_deny_last_reason(kCapabilityDenyReasonPolicyMissing)",
            "AC4: policy-missing site stamps PolicyMissing",
        ),
        (
            "note_capability_deny_last_reason(kCapabilityDenyReasonLimitsZero)",
            "AC4: limits-zero site stamps LimitsZero",
        ),
    )
    for needle, msg in sites:
        if needle not in cap:
            failures.append(msg)
    # AC5: unified hygiene atomic stamped with sentinel 7.
    if not re.search(r"g_macro_hygiene_last_limit_reason\.store\s*\(\s*7\s*,", cap):
        failures.append("AC5: unified g_macro_hygiene_last_limit_reason stamped with sentinel 7")
    return failures


def _check_no_direct_stores(me: str) -> list[str]:
    """AC6: no remaining direct store(2,...) or store(3,...) sites."""
    failures: list[str] = []
    for target, msg in (
        ("g_macro_hygiene_last_limit_reason.store(2,", "AC6: store(2,...) site still present (DepthLimit)"),
        ("g_macro_hygiene_last_limit_reason.store(3,", "AC6: store(3,...) site still present (PassLimit)"),
    ):
        if target in me:
            failures.append(msg)
    return failures


def run_strict() -> list[str]:
    me = _read("src/compiler/macro_expansion.cpp")
    ixx = _read("src/compiler/macro_expansion.ixx")
    cap = _read("src/core/capability_model.hh")
    failures: list[str] = []
    failures.extend(_check_ixx_sentinel(ixx))
    failures.extend(_check_case_seven(me))
    failures.extend(_check_capability_family(cap))
    failures.extend(_check_deny_sites(cap))
    failures.extend(_check_no_direct_stores(me))
    return failures


def _self_test() -> int:
    failures = run_strict()
    if failures:
        print("SELF-TEST FAIL:", file=sys.stderr)
        for f in failures:
            print("  -", f, file=sys.stderr)
        return 1
    print("SELF-TEST OK: all #3304 source-cite checks pass")
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
    print("OK: #3304 source-cite checks pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
