#!/usr/bin/env python3
"""Issue #3091 regression guard for TypeLinearCommitProof audit_mid."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CAP_HH = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
QRY_REFLECT = ROOT / "src" / "compiler" / "evaluator_primitives_query_reflect.cpp"
QRY_TYPE_STATS = ROOT / "src" / "compiler" / "evaluator_primitives_query_type_stats.cpp"


# Background: TypeLinearCommitProof is Agent's single commit evidence
# structure across densify / steal / remap but had no audit_mid. The
# #3091 fix appends audit_mid at struct END (per #2906 ABI discipline),
# stamps it from TLS boundary-noted mid / g_last_stamped_audit_mid at
# every build path, clears g_last_stamped_audit_mid in the abort
# clear path (#3030), and exposes audit-mid (+ schema-3091 /
# issue-3091 sentinels) on the three relevant query primitives:
#   - query:type-linear-commit-health
#   - query:type-linear-evolution-snapshot
#   - query:type-incremental-fidelity-stats (folded proof keys)
# This linter is the regression guard: presence checks for the field,
# the two build stamps, the abort clear, and the three query audit-mid
# keys. Exit 0 clean, exit 1 on any missing required pattern.


# Required patterns: presence checks. Use DOTALL so multi-line function
# bodies (with their own `{}` blocks) can match across newlines.
REQUIRED_PATTERNS = (
    # struct field (CAP_HH)
    (
        "struct field audit_mid",
        re.compile(
            r"struct\s+TypeLinearCommitProof\s*\{.*?"
            r"std::uint64_t\s+schema\s*=\s*2697\s*;"
            r".*?std::uint64_t\s+audit_mid\s*=\s*0\s*;.*?^\};",
            re.DOTALL | re.MULTILINE,
        ),
        1,
        CAP_HH,
    ),
    # build helpers stamp audit_mid (CAP_HH)
    (
        "build stamps audit_mid (live path)",
        re.compile(
            r"build_type_linear_commit_proof_from_live\b.*?"
            r"p\.audit_mid\s*=\s*g_tls_boundary_audit_noted.*?"
            r"g_last_stamped_audit_mid",
            re.DOTALL,
        ),
        1,
        CAP_HH,
    ),
    (
        "build stamps audit_mid (with_outcome path)",
        re.compile(
            r"build_type_linear_commit_proof_from_live_with_outcome\b.*?"
            r"p\.audit_mid\s*=\s*g_tls_boundary_audit_noted.*?"
            r"g_last_stamped_audit_mid",
            re.DOTALL,
        ),
        1,
        CAP_HH,
    ),
    # abort clear drops g_last_stamped_audit_mid (CAP_HH)
    (
        "abort clear drops g_last_stamped_audit_mid",
        re.compile(
            r"clear_type_linear_commit_proof_on_abort\s*\(\s*\)"
            r"\s*noexcept\s*\{.*?g_last_stamped_audit_mid\.store\(\s*0",
            re.DOTALL,
        ),
        1,
        CAP_HH,
    ),
    # query primitives expose audit-mid (QRY_REFLECT, QRY_TYPE_STATS)
    (
        "query_reflect audit-mid key",
        re.compile(r'"audit-mid"'),
        1,
        QRY_REFLECT,
    ),
    (
        "query_type_stats folded proof audit-mid key",
        re.compile(r'"type-linear-commit-proof-audit-mid"'),
        1,
        QRY_TYPE_STATS,
    ),
    # schema-3091 sentinel (aggregate across all targets)
    (
        "schema-3091 sentinel (aggregate)",
        re.compile(r'"schema-3091"\s*,\s*3091'),
        3,
        None,
    ),
)


def _strip_comments(text):
    """Strip // line comments and /* */ block comments. Keep string
    literals intact so query key strings ("audit-mid" etc.) remain
    visible to the regex."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def main(argv):
    parser = argparse.ArgumentParser(description="Issue #3091 regression guard")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 on any missing required pattern",
    )
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args(argv)

    targets = [CAP_HH, QRY_REFLECT, QRY_TYPE_STATS]
    for p in targets:
        if not p.exists():
            print(f"target not found: {p}", file=sys.stderr)
            return 2

    per_file = {p: _strip_comments(p.read_text(encoding="utf-8", errors="replace")) for p in targets}
    aggregate = "\n".join(per_file.values())

    presence = []
    missing = []
    for label, pat, min_count, path_hint in REQUIRED_PATTERNS:
        haystack = aggregate if path_hint is None else per_file[path_hint]
        count = len(pat.findall(haystack))
        presence.append({"label": label, "count": count, "min": min_count})
        if count < min_count:
            missing.append(f"{label}: found {count}, need >= {min_count}")

    ok = not missing
    result = {"presence": presence, "missing": missing, "ok": ok}

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        if missing:
            print("[FAIL] missing required patterns:")
            for m in missing:
                print(f"  {m}")
        if ok:
            print(
                "[OK] #3091 regression guard clean "
                "(struct audit_mid field + build stamps + abort clear + "
                "query audit-mid keys + schema-3091 sentinels all present)"
            )

    if args.strict and not ok:
        return 1
    return 0 if ok else (1 if args.strict else 0)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
