#!/usr/bin/env python3
"""Issue #4502 step 7: the value encoding has one source, lib/aura_value_abi.h.

Fails if src/ or lib/ reintroduce bias constants / range classifiers for
boxed floats and strings, or the legacy 0x10000000 string encoding, or if
the ABI consumers stop including/asserting the header.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
ABI = ROOT / "lib" / "aura_value_abi.h"
BANNED = [
    (re.compile(r"\b(FLOAT|STRING)_BIAS\w*\b"), "bias constant"),
    (re.compile(r"-?9000000000000000000LL"), "string bias literal"),
    (re.compile(r"-?10000000000000000LL"), "float bias literal"),
    (re.compile(r"\bSTRING_BIAS\s+0x10000000"), "legacy string encoding"),
]


def code_lines(text):
    for n, line in enumerate(text.splitlines(), 1):
        stripped = line.split("//", 1)[0]
        if stripped.lstrip().startswith(("*", "/*")):
            continue
        yield n, stripped


def main():
    errs = []
    if not ABI.exists():
        errs.append("lib/aura_value_abi.h missing")
    for base in ("src", "lib"):
        for p in (ROOT / base).rglob("*"):
            if p.suffix not in {".h", ".hh", ".c", ".cpp", ".ixx", ".hpp"} or p == ABI:
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
            for n, line in code_lines(text):
                for rx, what in BANNED:
                    if rx.search(line):
                        errs.append(f"{p.relative_to(ROOT)}:{n}: {what}: {line.strip()}")
    musts = {
        "src/compiler/value_tags.h": ["aura_value_abi.h", "kValueEncodingAbi = AURA_VALUE_ABI"],
        "lib/runtime.c": ['#include "aura_value_abi.h"', "_Static_assert(AURA_VALUE_ABI =="],
        "src/main.cpp": ["aura_value_abi.h", "AURA_ABI_STR(AURA_VALUE_ABI)"],
        "scripts/tools/release.sh": ["aura_value_abi.h"],
    }
    for rel, needles in musts.items():
        text = (ROOT / rel).read_text(encoding="utf-8", errors="replace")
        for nd in needles:
            if nd not in text:
                errs.append(f"{rel}: missing {nd!r}")
    if errs:
        print("FAIL #4502 single-source value encoding:")
        for e in errs:
            print("  " + e)
        return 1
    print("OK #4502 single-source value encoding")
    return 0


if __name__ == "__main__":
    sys.exit(main())
