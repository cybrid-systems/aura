#!/usr/bin/env python3
"""Issue #5192: std/list must not shadow the native foldl / take primitives.

The engine already implements both in C++ (src/compiler/evaluator_primitives_list.cpp:
a loop over the pair heap with the traversal counters). std/list redefined
them in Aura, and because the prelude auto-requires std/list, the interpreted
definitions were the DEFAULT fold and prefix for every user program:

  foldl over 1M elements   1448 ms (stdlib)  vs  20 ms (native)   ~72x
  take of a 500k prefix    1028 ms (stdlib)  vs  39 ms (native)   ~26x

take's Aura definition existed for a real reason - it enforced the SRFI-1
short-list error while the primitive clamped - so this gate also pins the
contract where it now lives: the primitive owns both the (lst k) order
(#4177) and the short-list error arm.

  AC1  lib/std/list.aura defines neither foldl nor take
  AC2  the export block still names both (the export resolves to the prims)
  AC3  evaluator_primitives_list.cpp registers both prims
  AC4  take keeps the SRFI-1 short-list error arm and the n > len guard
  AC5  the gate fails closed when it cannot see the shapes it polices

Run: python3 scripts/coverage/checks/check_stdlib_list_prim_shadow_5192.py [--self-test]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
LIST_REL = "lib/std/list.aura"
PRIMS_REL = "src/compiler/evaluator_primitives_list.cpp"

DEFINE_RE = r"\(\s*define\s*\(\s*(%s)[\s)]"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _redefinitions(source: str, names: tuple[str, ...]) -> list[str]:
    """Names defined with (define (name ... in source (comments stripped)."""
    stripped = re.sub(r";[^\n]*", "", source)
    found = []
    for name in names:
        if re.search(DEFINE_RE % re.escape(name), stripped):
            found.append(name)
    return found


def _export_block(source: str) -> str:
    start = source.find("(export")
    if start < 0:
        return ""
    depth = 0
    for i in range(start, len(source)):
        if source[i] == "(":
            depth += 1
        elif source[i] == ")":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
    return ""


def _check(lib: str, prims: str) -> list[str]:
    fails: list[str] = []

    names = ("foldl", "take")
    redefined = _redefinitions(lib, names)
    for name in redefined:
        fails.append(
            f"AC1: {LIST_REL} redefines {name} - that shadows the native primitive "
            "for every importer and the prelude auto-requires std/list (#5192)"
        )

    exports = _export_block(lib)
    if not exports:
        fails.append(f"AC1: {LIST_REL} has no (export ...) block - the gate cannot see the surface (#5192)")
    else:
        for name in names:
            if not re.search(r"(?<![A-Za-z0-9?!*/<>=+-])" + name + r"(?![A-Za-z0-9?!*/<>=+-])", exports):
                fails.append(
                    f"AC2: {LIST_REL}'s export block no longer names {name} - dropping the "
                    'name would silently remove it from (require "std/list" all:) (#5192)'
                )

    if not prims:
        fails.append(f"AC3: {PRIMS_REL} missing")
        return fails
    for name in names:
        if f'add, ev, "{name}"' not in prims:
            fails.append(f'AC3: {PRIMS_REL} does not register "{name}" (#5192)')

    # AC4: the take arm that makes the stdlib define unnecessary.
    if "take: list shorter than count" not in prims:
        fails.append(
            "AC4: take lost the SRFI-1 short-list error arm - the contract "
            "tests/suite/srfi_collections_5003.aura pins (#5192)"
        )
    if "if (n > len)" not in prims:
        fails.append("AC4: take lost the n > len guard that arms the short-list error (#5192)")
    return fails


def _self_test() -> int:
    """The matchers must catch the exact shape this issue removed."""
    good_lib = "(export foldl take)\n; foldl: native, do not redefine\n"
    good_prims = 'add, ev, "foldl"\nadd, ev, "take"\n"take: list shorter than count"\nif (n > len)'
    fails = []
    if _check(good_lib, good_prims):
        fails.append("self-test: a clean module was rejected")

    bad_lib = good_lib + "(define (foldl f init lst) (reverse lst))\n"
    if "AC1" not in " ".join(_check(bad_lib, good_prims)):
        fails.append("self-test: a shadowing (define (foldl ...)) slipped past")

    bad_lib2 = good_lib + "(define ( take lst n) (list))\n"
    if "AC1" not in " ".join(_check(bad_lib2, good_prims)):
        fails.append("self-test: a shadowing (define ( take ...)) slipped past")

    no_export = "; just a comment\n"
    if "AC1" not in " ".join(_check(no_export, good_prims)):
        fails.append("self-test: a module with no export block was accepted")

    for f in fails:
        print(f"FAIL: {f}", file=sys.stderr)
    if fails:
        return 1
    print("OK #5192 self-test: the stdlib shadow gate catches the regression")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Issue #5192 stdlib list prim-shadow gate")
    ap.add_argument("--self-test", action="store_true", help="run the built-in probes")
    args = ap.parse_args(argv)
    if args.self_test:
        return _self_test()

    lib = _read(LIST_REL)
    prims = _read(PRIMS_REL)
    if not lib:
        print(f"FAIL #5192: missing source {LIST_REL}", file=sys.stderr)
        return 1
    fails = _check(lib, prims)
    if fails:
        for f in fails:
            print(f"FAIL #5192: {f}", file=sys.stderr)
        return 1
    print("OK #5192 stdlib_shadow: std/list exports the native foldl/take and redefines neither")
    return 0


if __name__ == "__main__":
    sys.exit(main())
