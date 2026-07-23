#!/usr/bin/env python3
"""Issue #1886: naming_convention.md presence + required sections + example files.

Lightweight gate helper (not a full codebase style linter). Verifies:
  1. docs/naming_convention.md exists
  2. Required section headings are present
  3. At least MIN_TEMPLATE_FILES core sources contain the canonical
     comment template keys (Purpose: + AI-Native Rationale:)

Usage:
  python3 scripts/check_naming_convention.py
  python3 scripts/check_naming_convention.py --soft   # warn but exit 0

Exit 0 = OK, 1 = violation.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "naming_convention.md"

REQUIRED_HEADINGS = (
    "## 1. General principles",
    "## 2. Modules & files",
    "## 3. Types, values, functions",
    "## 4. Primitive & metrics naming",
    "## 5. Comment templates",
    "## 6. AI-Native rationale standards",
    "PrimMeta",
    "Purpose:",
    "Safety Class:",
    "AI-Native Rationale:",
)

# Canonical example files updated under #1886 (must keep template keys).
EXAMPLE_FILES = (
    "src/core/module_boundary.ixx",
    "src/core/concepts.ixx",
    "src/core/concept_constraints.ixx",
    "src/compiler/typed_mutation_audit.h",
    "src/core/mutation.ixx",
    "src/core/error.ixx",
)

MIN_TEMPLATE_FILES = 5
TEMPLATE_KEYS = ("Purpose:", "AI-Native Rationale:")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--soft", action="store_true", help="warn but exit 0 on violation")
    args = ap.parse_args()

    errors: list[str] = []

    if not DOC.is_file():
        # 2026-07-23: aura is agent-developed; per Anqi's "no docs" philosophy,
        # docs/* was deleted (kept only docs/generated/). The naming-convention
        # doc itself is gone, but the structural check on example-file template
        # keys below is still enforced -- the doc presence check becomes soft.
        # Future re-introduction of the doc would resume full heading checks.
        print(f"note: {DOC.relative_to(ROOT)} not present (soft-skipping doc heading check)", file=sys.stderr)
    else:
        text = DOC.read_text(encoding="utf-8", errors="replace")
        for h in REQUIRED_HEADINGS:
            if h not in text:
                errors.append(f"docs/naming_convention.md missing required text: {h!r}")

    hits = 0
    missing_examples: list[str] = []
    for rel in EXAMPLE_FILES:
        path = ROOT / rel
        if not path.is_file():
            missing_examples.append(f"missing example file: {rel}")
            continue
        body = path.read_text(encoding="utf-8", errors="replace")
        if all(k in body for k in TEMPLATE_KEYS):
            hits += 1
        else:
            missing_examples.append(f"{rel}: missing template keys {TEMPLATE_KEYS}")

    if hits < MIN_TEMPLATE_FILES:
        errors.append(f"only {hits} example files have Purpose:+AI-Native Rationale: (need >= {MIN_TEMPLATE_FILES})")
        errors.extend(missing_examples)

    if errors:
        msg = "FAIL: naming convention check (#1886):\n" + "".join(f"  - {e}\n" for e in errors)
        print(msg, file=sys.stderr)
        return 0 if args.soft else 1

    print(f"OK: naming_convention.md ({hits} example files with Purpose/AI-Native template keys)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
