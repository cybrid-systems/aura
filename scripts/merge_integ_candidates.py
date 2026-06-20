#!/usr/bin/env python3
"""Merge extract_integ_from_issues.py candidates into integ_tests.json."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INTEG = ROOT / "tests" / "fixtures" / "integ_tests.json"
EXTRACT = ROOT / "scripts" / "extract_integ_from_issues.py"


def main() -> int:
    raw = subprocess.check_output([sys.executable, str(EXTRACT), "--json"], text=True)
    candidates = json.loads(raw)
    existing = json.loads(INTEG.read_text(encoding="utf-8"))
    names = {e["name"] for e in existing}
    added = 0
    for c in candidates:
        if c.get("expected") is None:
            continue
        if c["name"] in names:
            continue
        existing.append(
            {
                "name": c["name"],
                "code": c["code"],
                "pipeline": c.get("pipeline", "eval"),
                "expected": c["expected"],
            }
        )
        names.add(c["name"])
        added += 1
    INTEG.write_text(json.dumps(existing, indent=2) + "\n", encoding="utf-8")
    print(f"added {added} integ cases ({len(existing)} total)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
