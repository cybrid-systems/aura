#!/usr/bin/env python3
"""Source invariant: one owner for FNV-1a and hash-slot metadata (#901/#908).

The #901/#908 campaign put the FNV-1a constants and the linear-probe slot
rule in src/compiler/hash_meta.h. New code kept re-introducing both as raw
literals, so this test pins the invariant: outside hash_meta.h there are no
raw FNV constants and no inline "h >> 57" fingerprint expression.

Module interface units are a documented exception: textually including
hash_meta.h in their global module fragment collides with importers that
include query_result_decode.hh (redefinition of hash::fingerprint), so they
still carry the literals until a module-owned helper exists.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
CANONICAL = SRC / "compiler" / "hash_meta.h"
EXTS = {".cpp", ".cc", ".cxx", ".hh", ".hpp", ".h", ".ixx"}
SKIP_DIRS = {"linenoise", "__pycache__"}

FNV_LITERALS = (
    re.compile(r"0xcbf29ce484222325", re.IGNORECASE),
    re.compile(r"0x100000001b3", re.IGNORECASE),
    re.compile(r"(?<![\w.])1099511628211(?![\w.])"),
    re.compile(r"(?<![\w.])14695981039346656037(?![\w.])"),
)
FINGERPRINT_RULE = re.compile(r">>\s*57")
# Decimal ASCII codes for the digit / upper / lower ranges. Character
# literals make the intent visible and match the value type.
ASCII_RANGE = re.compile(r">=\s*(?:48|65|97)\b[^;\n]*<=\s*(?:57|90|122)\b")

MODULE_EXCEPTIONS = {
    "src/core/ast.ixx",
    "src/compiler/evaluator.ixx",
    "src/compiler/type_checker.ixx",
    "src/compiler/optimization_passes.ixx",
}


def code_lines(path: Path) -> list[str]:
    """Lines with C/C++ comments blanked (string literals preserved)."""
    text = path.read_text(encoding="utf-8", errors="replace")
    out: list[str] = []
    in_block = False
    for raw in text.splitlines():
        line = []
        i = 0
        in_string = False
        while i < len(raw):
            ch = raw[i]
            nxt = raw[i + 1] if i + 1 < len(raw) else ""
            if in_block:
                if ch == "*" and nxt == "/":
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if in_string:
                line.append(ch)
                if ch == "\\":
                    if nxt:
                        line.append(nxt)
                        i += 2
                        continue
                elif ch == '"':
                    in_string = False
                i += 1
                continue
            if ch == "/" and nxt == "/":
                break
            if ch == "/" and nxt == "*":
                in_block = True
                i += 2
                continue
            if ch == '"':
                in_string = True
            line.append(ch)
            i += 1
        out.append("".join(line))
    return out


def source_files() -> list[Path]:
    out: list[Path] = []
    for path in SRC.rglob("*"):
        if not path.is_file() or path.suffix not in EXTS:
            continue
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        out.append(path)
    return out


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


class MagicConstantsInvariant(unittest.TestCase):
    def test_fingerprint_rule_only_in_hash_meta(self) -> None:
        offenders = []
        for path in source_files():
            if path == CANONICAL or rel(path) in MODULE_EXCEPTIONS:
                continue
            for lineno, line in enumerate(code_lines(path), 1):
                if FINGERPRINT_RULE.search(line):
                    offenders.append(f"{rel(path)}:{lineno}: {line.strip()}")
        self.assertEqual(offenders, [], "inline hash-slot fingerprint rule found")

    def test_ascii_classification_uses_char_literals(self) -> None:
        offenders = []
        for path in source_files():
            for lineno, line in enumerate(code_lines(path), 1):
                if ASCII_RANGE.search(line):
                    offenders.append(f"{rel(path)}:{lineno}: {line.strip()}")
        self.assertEqual(offenders, [], "ASCII range compared as magic decimals")

    def test_fnv_literals_only_in_hash_meta(self) -> None:
        offenders = []
        for path in source_files():
            if path == CANONICAL or rel(path) in MODULE_EXCEPTIONS:
                continue
            for lineno, line in enumerate(code_lines(path), 1):
                if any(rx.search(line) for rx in FNV_LITERALS):
                    offenders.append(f"{rel(path)}:{lineno}: {line.strip()}")
        self.assertEqual(offenders, [], "raw FNV-1a constants found")


if __name__ == "__main__":
    unittest.main(verbosity=2)
