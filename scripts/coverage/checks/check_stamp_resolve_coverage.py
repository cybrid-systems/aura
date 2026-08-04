#!/usr/bin/env python3
"""Issue #2224: Agent-facing StableNodeRef export must route through
``export_ref`` / ``make_stamped_*`` so tenant + fiber stamp is guaranteed.

Scans ``src/compiler/evaluator_primitives*.cpp`` for ``add("prim:name", …)``
registrations whose names look Agent-facing (ast:*, query:*) and whose
lambda body calls ``workspace_flat_->make_ref(`` / ``ws->make_ref(`` /
``->make_safe_ref(`` directly (i.e. without first going through
``ev.export_ref`` or one of the ``ev.make_stamped_*`` helpers).

This is the static / CI gate for Phase A of #2224 — mirrors
``check_side_effect_security.py`` (which guards Phase A of #2152 dispatch
required_effects). The test ``test_isolation_stamp_resolve`` (AC1)
covers the runtime check; this script locks the source-cite pattern so
future Agent-facing prims can't regress.

Per-prim allowlist lives in
``tests/stamp-resolve-coverage-allowlist.txt`` (one prim name per line,
``#`` comments). Use it sparingly — every entry should cite why a
direct ``make_ref`` is acceptable (typically: the ref is internal
audit provenance, not handed to Agent; or the prim is intentionally
in a non-Agent debug surface).

Usage:
  python3 scripts/coverage/checks/check_stamp_resolve_coverage.py              # report
  python3 scripts/coverage/checks/check_stamp_resolve_coverage.py --strict    # exit 1 on violations
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PRIM_GLOB = "src/compiler/evaluator_primitives*.cpp"
ALLOWLIST_PATH = ROOT / "tests" / "stamp-resolve-coverage-allowlist.txt"

# Prim families that may hand a StableNodeRef to Agent / user code.
# mutate: family returns bool / void to Agent — internal ``make_ref``
# for audit provenance is allowed.
AGENT_FACING_FAMILIES = ("ast:", "query:")

# Calls that build a raw StableNodeRef. Phase A requires these be
# routed through ``ev.export_ref`` / ``ev.make_stamped_ref`` /
# ``ev.make_stamped_safe_ref`` when inside an Agent-facing prim.
RAW_MAKE_REF_RE = re.compile(r"(?:workspace_flat_->make_ref\(|->make_ref\(|->make_safe_ref\()")
# Calls that are explicitly stamped — the "safe" call sites that
# pass the check.
STAMPED_HELPERS = (
    "ev.export_ref(",
    "ev.export_ref_safe(",
    "ev.export_held_ref(",
    "ev.finalize_agent_export(",
    "ev.make_stamped_ref(",
    "ev.make_stamped_safe_ref(",
)

# Issue #2404: Agent export return-path helpers that force
# validate_or_refresh before handoff.
EXPORT_VALIDATE_HELPERS = (
    "ev.export_ref(",
    "ev.export_ref_safe(",
    "ev.export_held_ref(",
    "ev.finalize_agent_export(",
    "ensure_valid_or_refresh(",
    "validate_or_refresh(",
)

# Prim names that *return* StableNodeRef-shaped values to Agents and
# therefore must call one of EXPORT_VALIDATE_HELPERS (#2404 AC1).
# children-stable uses for_each_stable_child (fresh live-gen capture at
# walk time — documented #2404 soft path; not in this list).
EXPORT_RETURN_PRIMS = (
    "query:stable-ref",
    "query:ensure-ref",
    "query:parent-stable",
    "ast:stable-ref",
)


def load_allowlist(path: Path) -> set[str]:
    if not path.exists():
        return set()
    allow: set[str] = set()
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        allow.add(line)
    return allow


def find_agent_prims(path: Path) -> list[tuple[str, int, str]]:
    """Return ``(prim_name, line_no, body)`` for each Agent-facing prim.

    A prim is "Agent-facing" if its name starts with one of the
    ``AGENT_FACING_FAMILIES`` prefixes. We scan the file linearly and
    pick up the lambda body between the matching ``add("…", …) {`` and
    the next top-level ``});`` (paren-balanced heuristically; sufficient
    for the well-behaved codebase).
    """
    text = path.read_text()
    results: list[tuple[str, int, str]] = []
    lines = text.splitlines()
    # Match ``add("prim:name", …) -> EvalValue {`` and similar.
    add_re = re.compile(r'\badd\(\s*"(' + "|".join(re.escape(p) for p in AGENT_FACING_FAMILIES) + r'[^"]+)"')
    i = 0
    while i < len(lines):
        m = add_re.search(lines[i])
        if not m:
            i += 1
            continue
        prim_name = m.group(1)
        # Find the opening ``{`` of the lambda body on this or the next
        # few lines (the capture may wrap).
        depth = 0
        j = i
        opened = False
        while j < len(lines):
            for ch in lines[j]:
                if ch == "{":
                    depth += 1
                    opened = True
                elif ch == "}":
                    depth -= 1
                    if opened and depth == 0:
                        # End of the outer lambda body. Capture lines
                        # ``i..=j`` as the body, with line numbers
                        # 1-based.
                        body = "\n".join(lines[i : j + 1])
                        results.append((prim_name, i + 1, body))
                        i = j + 1
                        break
            else:
                j += 1
                continue
            break
        else:
            # Unbalanced; bail to avoid infinite loop.
            i += 1
    return results


def find_raw_make_refs(body: str) -> list[tuple[int, str]]:
    """Return ``(line_offset, snippet)`` for each raw ``make_ref`` /
    ``make_safe_ref`` call in ``body`` that is NOT preceded by one of
    the STAMPED_HELPERS in the enclosing scope.

    The "preceded by" check is simple: a line with the raw call is
    flagged UNLESS one of the stamped helpers appears on the same
    line. (Multi-line: not flagged — we err on the side of letting
    legitimate multi-line stamp+make_ref combos through. Most call
    sites in this codebase are single-line.)
    """
    out: list[tuple[int, str]] = []
    for offset, line in enumerate(body.splitlines()):
        if not RAW_MAKE_REF_RE.search(line):
            continue
        if any(h in line for h in STAMPED_HELPERS):
            continue
        out.append((offset, line.strip()))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 on any violation (CI gate). Default: report only.",
    )
    args = parser.parse_args()

    allowlist = load_allowlist(ALLOWLIST_PATH)
    violations: list[tuple[Path, str, int, str]] = []
    export_missing: list[tuple[Path, str, int]] = []
    scanned = 0

    for path in sorted(ROOT.glob(PRIM_GLOB)):
        if not path.is_file():
            continue
        scanned += 1
        for prim_name, start_line, body in find_agent_prims(path):
            if prim_name in allowlist:
                continue
            for offset, snippet in find_raw_make_refs(body):
                violations.append((path, prim_name, start_line + offset, snippet))
            # Issue #2404 AC1: return-path export prims must validate.
            if prim_name in EXPORT_RETURN_PRIMS and not any(h in body for h in EXPORT_VALIDATE_HELPERS):
                export_missing.append((path, prim_name, start_line))

    print(f"scanned {scanned} prim TU(s) under {PRIM_GLOB}")
    ok_stamp = not violations
    ok_export = not export_missing
    if ok_stamp:
        print("OK — all Agent-facing prims route through export_ref / make_stamped_*")
    else:
        print(f"FOUND {len(violations)} raw make_ref violation(s):")
        for path, prim_name, line_no, snippet in violations:
            rel = path.relative_to(ROOT)
            print(f"  {rel}:{line_no}  {prim_name}  →  {snippet}")
    if ok_export:
        print("OK — #2404 export return-path prims call validate/export helpers")
    else:
        print(f"FOUND {len(export_missing)} #2404 export-return coverage gap(s):")
        for path, prim_name, line_no in export_missing:
            rel = path.relative_to(ROOT)
            print(f"  {rel}:{line_no}  {prim_name}  missing export/validate helper")
    if ok_stamp and ok_export:
        return 0
    print(f"\nAllowlist (if a violation is intentional): {ALLOWLIST_PATH.relative_to(ROOT)}")
    return 1 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
