#!/usr/bin/env python3
"""Rewrite manifest path pins when a test or source file moves.

Manifest rows cite a path and a substring (often a test or case id). They
do not cite a character window. A merge updates the path here instead of
editing slices by hand.

Dry-run unless `--apply` is set.

  python3 scripts/coverage/retarget_pins.py --from tests/old.cpp --to tests/new.cpp
  python3 scripts/coverage/retarget_pins.py --from tests/old.cpp --to tests/new.cpp --apply
  python3 scripts/coverage/retarget_pins.py --list-test-pins
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFESTS = ROOT / "scripts" / "coverage" / "manifests"


def _rows(data: dict) -> list[dict]:
    return list(data.get("checks") or [])


def _rewrite(value: object, src: str, dst: str) -> tuple[object, int]:
    if isinstance(value, str):
        if value == src:
            return dst, 1
        return value, 0
    if isinstance(value, list):
        count = 0
        out = []
        for item in value:
            new, n = _rewrite(item, src, dst)
            count += n
            out.append(new)
        return out, count
    return value, 0


def retarget(src: str, dst: str, *, apply: bool) -> int:
    changed = 0
    for path in sorted(MANIFESTS.glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        count = 0
        for check in _rows(data):
            for key in ("path", "paths", "forbid"):
                if key not in check:
                    continue
                new, n = _rewrite(check[key], src, dst)
                count += n
                check[key] = new
        if not count:
            continue
        changed += count
        print(f"{path.name}: {count} pin(s) {src} -> {dst}")
        if apply:
            path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"{changed} pin(s){' written' if apply else ' (dry-run)'}")
    return 0


def list_test_pins() -> int:
    rows = 0
    for path in sorted(MANIFESTS.glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        for check in _rows(data):
            targets = []
            if isinstance(check.get("path"), str):
                targets.append(check["path"])
            targets.extend(p for p in (check.get("paths") or []) if isinstance(p, str))
            test_targets = [t for t in targets if t.startswith("tests/")]
            if not test_targets:
                continue
            rows += 1
            needles = check.get("contains") or check.get("contains_any") or []
            print(f"{path.stem}\t{check.get('ac', '')}\t{','.join(test_targets)}\t{len(needles)}")
    print(f"{rows} test-file row(s)", file=sys.stderr)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--from", dest="src", help="Repo-relative path to replace")
    parser.add_argument("--to", dest="dst", help="Repo-relative replacement path")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--list-test-pins", action="store_true")
    args = parser.parse_args()
    if args.list_test_pins:
        return list_test_pins()
    if not args.src or not args.dst:
        parser.error("--from and --to are required unless --list-test-pins")
    return retarget(args.src, args.dst, apply=args.apply)


if __name__ == "__main__":
    sys.exit(main())
