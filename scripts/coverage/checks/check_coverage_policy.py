#!/usr/bin/env python3
"""Gate: coverage checks stay custom-logic; substring contracts are manifests.

See tests/COVERAGE.md. Fails if:
  - a coverage/checks/check_*.py is a thin runner.py wrapper
  - a coverage/checks/check_*.py is substring-only (`must("x" in file)`)
  - a new scripts/check_*.py appears outside the frozen root-scripts set
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CHECKS = ROOT / "scripts" / "coverage" / "checks"
ROOT_SCRIPTS = ROOT / "scripts"
POLICY = Path(__file__).name

# Frozen: do not add scripts/check_*.py. New static gates go under
# scripts/coverage/checks/ (custom logic) or manifests/ (substring).


def _allowlist() -> set[str]:
    p = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"
    if p.is_file():
        return {
            ln.strip() for ln in p.read_text(encoding="utf-8").splitlines() if ln.strip() and not ln.startswith("#")
        }
    return set()


def classify(text: str) -> str:
    if (
        "runner.py" in text
        and "--issue" in text
        and text.count("def ") <= 3
        and "must(" not in text.replace("_must(", "_X(")
    ):
        return "wrapper"
    if any(
        m in text
        for m in (
            "re.compile",
            "re.search",
            "re.findall",
            "re.match",
            "ast.parse",
        )
    ):
        return "custom"
    if "subprocess." in text and "runner.py" not in text:
        return "custom"
    if ".find(" in text and re.search(r"\[[^\]]+:[^\]]+\]", text):
        return "custom"
    if text.count("must(") >= 2:
        return "simple"
    return "other"


def main() -> int:
    fails: list[str] = []
    gf_path = ROOT / "scripts" / "coverage" / "simple_check_grandfather.txt"
    grandfather = set()
    if gf_path.is_file():
        grandfather = {
            ln.strip()
            for ln in gf_path.read_text(encoding="utf-8").splitlines()
            if ln.strip() and not ln.startswith("#")
        }

    for path in sorted(CHECKS.glob("check_*.py")):
        if path.name == POLICY:
            continue
        kind = classify(path.read_text(encoding="utf-8", errors="replace"))
        if kind == "wrapper":
            fails.append(
                f"{path.relative_to(ROOT)}: thin runner.py wrapper — delete it; "
                "gate runs manifests via run_checks.py (tests/COVERAGE.md)"
            )
        elif kind == "simple" and path.name not in grandfather:
            fails.append(
                f"{path.relative_to(ROOT)}: substring-only check — fold into "
                "scripts/coverage/manifests/<N>.json (tests/COVERAGE.md)"
            )

    allow = _allowlist()
    names = {p.name for p in ROOT_SCRIPTS.glob("check_*.py")}
    extra = sorted(names - allow)
    missing = sorted(allow - names)
    if extra:
        fails.append(
            "new scripts/check_*.py not on the frozen allowlist: "
            + ", ".join(extra)
            + " — put custom gates in scripts/coverage/checks/ or a manifest"
        )
    if missing:
        fails.append(
            "scripts/check_*.py allowlist stale (file removed without updating "
            "scripts/coverage/root_check_allowlist.txt): " + ", ".join(missing)
        )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} coverage-policy row(s) failed", file=sys.stderr)
        return 1
    print("OK: coverage policy — no new substring-only/wrapper checks; root scripts frozen")
    return 0


if __name__ == "__main__":
    sys.exit(main())
