#!/usr/bin/env python3
"""corpus_tools.py — corpus inventory / sync helpers (Issue #1935).

Usage:
  python3 tests/fuzz/corpus_tools.py status
  python3 tests/fuzz/corpus_tools.py list --limit 20
  python3 tests/fuzz/corpus_tools.py sync-repro   # copy .aura reproducers → corpus seeds
"""

from __future__ import annotations

import argparse
import hashlib
import sys

from common import CORPUS, REPRODUCERS


def status() -> int:
    files = sorted(CORPUS.glob("*.sexpr")) if CORPUS.is_dir() else []
    repro = list(REPRODUCERS.rglob("*.aura")) if REPRODUCERS.is_dir() else []
    total_bytes = sum(f.stat().st_size for f in files)
    print(f"corpus dir : {CORPUS}")
    print(f"  seeds    : {len(files)}")
    print(f"  bytes    : {total_bytes}")
    print(f"reproducers: {REPRODUCERS} ({len(repro)} .aura)")
    return 0


def list_seeds(limit: int) -> int:
    files = sorted(CORPUS.glob("*.sexpr")) if CORPUS.is_dir() else []
    for f in files[:limit]:
        print(f"{f.name}\t{f.stat().st_size}")
    if len(files) > limit:
        print(f"... {len(files) - limit} more")
    return 0


def sync_repro() -> int:
    """Copy unique reproducer contents into corpus as repro_*.sexpr."""
    REPRODUCERS.mkdir(parents=True, exist_ok=True)
    CORPUS.mkdir(parents=True, exist_ok=True)
    added = 0
    for src in sorted(REPRODUCERS.rglob("*.aura")):
        data = src.read_bytes()
        if not data.strip():
            continue
        h = hashlib.sha1(data).hexdigest()[:12]
        dst = CORPUS / f"repro_{h}.sexpr"
        if dst.exists():
            continue
        dst.write_bytes(data)
        added += 1
        print(f"  + {dst.name} from {src.relative_to(REPRODUCERS.parent.parent)}")
    print(f"sync-repro: added {added} seed(s)")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    p_list = sub.add_parser("list")
    p_list.add_argument("--limit", type=int, default=50)
    sub.add_parser("sync-repro")
    args = ap.parse_args(argv)
    if args.cmd == "status":
        return status()
    if args.cmd == "list":
        return list_seeds(args.limit)
    if args.cmd == "sync-repro":
        return sync_repro()
    return 2


if __name__ == "__main__":
    sys.exit(main())
