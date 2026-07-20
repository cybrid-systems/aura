#!/usr/bin/env python3
"""Fixture maintenance tooling (#1962).

Commands:
  validate   Run schema/budget checks (same as tests/python/fixture_check.py)
  status     Print shard counts and sizes
  pack KIND  Merge shards → stdout JSON array (export / debugging only)

Hand-edit shards under tests/fixtures/<kind>/*.json — do not reintroduce
large mono *_tests.json files.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
sys.path.insert(0, str(TESTS))

from fixture_check import run_check  # noqa: E402
from fixture_store import fixture_status, load_case_array  # noqa: E402


def cmd_status() -> int:
    rows = fixture_status()
    print(f"{'kind':12s} {'mode':8s} {'files':>5s} {'cases':>6s} {'bytes':>8s}")
    for r in rows:
        print(f"{r['kind']:12s} {r['mode']:8s} {r.get('files', 0):5d} {r.get('cases', 0):6d} {r.get('bytes', 0):8d}")
        if r.get("mode") == "shards" and r.get("shards"):
            for name, n, sz in r["shards"]:
                flag = " !" if sz > 12_000 or n > 50 else ""
                print(f"    {name:28s} {n:4d} cases {sz:6d} B{flag}")
    return 0


def cmd_pack(kind: str) -> int:
    cases = load_case_array(kind)
    json.dump(cases, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write("\n")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("validate", help="schema + budget checks")
    sub.add_parser("status", help="shard inventory")
    p_pack = sub.add_parser("pack", help="merge shards to stdout JSON array")
    p_pack.add_argument("kind", choices=("regression", "integ", "benchmark", "smoke"))
    args = ap.parse_args(argv)
    if args.cmd == "validate":
        return run_check()
    if args.cmd == "status":
        return cmd_status()
    if args.cmd == "pack":
        return cmd_pack(args.kind)
    return 2


if __name__ == "__main__":
    sys.exit(main())
