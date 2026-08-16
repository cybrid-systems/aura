#!/usr/bin/env python3
"""Issue #3090: regression guard for the mid-fallback refuse vs grant
synthesized-mid alignment fix.

Background
----------
``CapabilityRegistry::grant`` / ``grant_macro_self_evo`` previously
synthesized ``bound_mid = grant_epoch ?: 1`` under Restricted/Strict to
placate the #2531 mid-join hole guard. That synthesis produced phantom
mid=1 entries which could not be joined with the typed-trail /
SE mid=0 refuse events emitted by ``resolve_audit_mutation_id`` (#2836).
``make_grant_provenance`` had a parallel ``epoch ?: 1`` fallback;
``record_audit`` SE mid computation had the same ``epoch ?: 1`` chain.

The #3090 fix removes both synthesis paths from production code paths.
This linter is the regression guard:

  * FORBIDDEN — the removed patterns must NOT reappear in
    ``src/core/capability_model.hh``. They are now anti-patterns.
  * PRESENCE — the pre-lock refuse block
    (``prov.mutation_id == 0`` under fail_closed) must exist in
    ``grant()`` so a future refactor cannot accidentally drop the refuse
    without noticing.
  * ALLOWED — the Soft/Off session_bound synthesis
    ``bound_mutation_id = grant_epoch != 0 ? grant_epoch : 1`` remains
    (zero-cost contract, AC5) and is not flagged.

Exit codes:
  0 — clean (no removed patterns; refuse block present)
  1 — at least one anti-pattern found OR refuse block missing
  2 — invocation error

Usage:
  python3 scripts/check_grant_mid_refused_3090.py            # report
  python3 scripts/check_grant_mid_refused_3090.py --strict    # exit 1 on hit
  python3 scripts/check_grant_mid_refused_3090.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# File under audit. The fix touches capability_model.hh (grants) and
# typed_mutation_audit.h (resolve refuse SSOT) — both stay in scope for
# regression coverage.
TARGET = ROOT / "src" / "core" / "capability_model.hh"

# Anti-patterns removed by the #3090 fix. These MUST NOT appear in
# capability_model.hh after the fix ships. Each pattern is the literal
# shape that previously synthesized a phantom mid=1 / mid=epoch audit key.
FORBIDDEN_PATTERNS: tuple[re.Pattern[str], ...] = (
    # make_grant_provenance fallback: prov.mutation_id = prov.epoch ?: 1
    re.compile(
        r"if\s*\(\s*prov\.mutation_id\s*==\s*0\s*\)\s*"
        r"prov\.mutation_id\s*=\s*prov\.epoch\s*!=\s*0\s*\?\s*prov\.epoch\s*:\s*1\b"
    ),
    # record_audit SE mid chain: prov.mutation_id != 0 ? prov.mutation_id
    #                                : (prov.epoch != 0 ? prov.epoch : 1)
    re.compile(
        r"prov\.mutation_id\s*!=\s*0\s*\?\s*prov\.mutation_id\s*:\s*"
        r"\(\s*prov\.epoch\s*!=\s*0\s*\?\s*prov\.epoch\s*:\s*"
        r"(?:static_cast<std::uint64_t>\(1\)|1)\s*\)"
    ),
    # record_audit SE mid chain (compact form, single line, no static_cast):
    re.compile(
        r"prov\.mutation_id\s*!=\s*0\s*\?\s*prov\.mutation_id\s*:\s*"
        r"\(\s*prov\.epoch\s*!=\s*0\s*\?\s*prov\.epoch\s*:\s*1\s*\)"
    ),
)

# Presence checks. After the fix, capability_model.hh must contain:
#   * the pre-lock refuse block in grant() — checks fail_closed mid
#   * the refuse counter fetch_add — at least twice (grant +
#     grant_macro_self_evo, or one with a comment-tagged defensive copy)
#   * the SE reason string "grant-mid-refused" — emitted from refuse path
REQUIRED_PATTERNS: tuple[tuple[str, re.Pattern[str], int], ...] = (
    (
        "pre-lock refuse gate",
        re.compile(
            r"fail_closed\s*&&\s*prov\.mutation_id\s*==\s*0"
        ),
        1,  # at least once
    ),
    (
        "refuse counter fetch_add",
        re.compile(
            r"capability_grant_mid_refused_total\.fetch_add"
        ),
        2,  # grant + grant_macro_self_evo
    ),
    (
        "SE reason 'grant-mid-refused'",
        re.compile(
            r'"grant-mid-refused"'
        ),
        2,  # grant + grant_macro_self_evo
    ),
)


def _strip_comments_and_strings(text: str) -> str:
    """Strip ``//`` line comments and ``/* */`` block comments so pattern
    matches reflect executable code only.

    We keep string literals (``"..."``) intact — the SE reason string
    ``"grant-mid-refused"`` is a positive signal that the refuse path
    emits, and that is code-shape, not commentary.
    """

    # Block comments first (greedy non-overlapping).
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    # Line comments — preserve the line break so line numbers stay sane.
    text = re.sub(r"//[^\n]*", "", text)
    return text


def scan(path: Path) -> dict:
    raw = path.read_text(encoding="utf-8", errors="replace")
    stripped = _strip_comments_and_strings(raw)
    forbidden_hits: list[dict] = []
    for pat in FORBIDDEN_PATTERNS:
        for m in pat.finditer(stripped):
            line_no = stripped.count("\n", 0, m.start()) + 1
            forbidden_hits.append(
                {
                    "pattern": pat.pattern,
                    "line": line_no,
                    "match": m.group(0)[:120],
                }
            )
    presence: list[dict] = []
    missing: list[str] = []
    for label, pat, min_count in REQUIRED_PATTERNS:
        count = len(pat.findall(stripped))
        presence.append({"label": label, "count": count, "min": min_count})
        if count < min_count:
            missing.append(
                f"{label}: found {count}, need >= {min_count}"
            )
    return {
        "file": str(path.relative_to(ROOT)),
        "forbidden_hits": forbidden_hits,
        "presence": presence,
        "missing": missing,
        "ok": not forbidden_hits and not missing,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 on any forbidden pattern or missing required pattern",
    )
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args(argv)

    if not TARGET.exists():
        print(f"target not found: {TARGET}", file=sys.stderr)
        return 2

    result = scan(TARGET)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        rel = result["file"]
        if result["forbidden_hits"]:
            print(f"[FAIL] {rel}: forbidden anti-patterns reintroduced:")
            for h in result["forbidden_hits"]:
                print(
                    f"  line {h['line']}: {h['match']}"
                )
                print(f"    pattern: {h['pattern']}")
        if result["missing"]:
            print(f"[FAIL] {rel}: missing required patterns:")
            for m in result["missing"]:
                print(f"  {m}")
        if result["ok"]:
            print(
                f"[OK] {rel}: #3090 regression guard clean "
                f"(no forbidden patterns; all required presence checks pass)"
            )

    if args.strict and not result["ok"]:
        return 1
    return 0 if result["ok"] else (1 if args.strict else 0)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
