#!/usr/bin/env python3
"""json-escape / json-obj-items / json-arr-items must be strict JSON.

Aura's json-parse accepts raw control characters, so a round-trip through
it is not evidence. These snippets are checked with Python's json.loads.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def _run(source: str) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_PATH"] = str(ROOT / "lib")
    env.pop("AURA_SANDBOX", None)
    env["AURA_PIPELINE_STRICT"] = "0"
    try:
        return subprocess.run(
            [str(AURA_BIN), "--preset", "dev", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=60,
        )
    finally:
        os.unlink(path)


CASES = {
    "newline": r'(display (string-append "\"" (json-escape "line1\nline2") "\""))',
    "tab": r'(display (string-append "\"" (json-escape "a\tb") "\""))',
    "quote-backslash": r'(display (string-append "\"" (json-escape "a\"b\\c") "\""))',
    "u0001": r'(display (json-encode "\x01"))',
    "obj": ('(display (string-append "{" (json-obj-items (hash "a\\nb" 1) (list "a\\nb") number->string) "}"))'),
    "arr": (
        '(display (string-append "[" '
        '(json-arr-items (list "a\\nb" "c\\"d") '
        '(lambda (s) (string-append "\\"" (json-escape s) "\\""))) "]"))'
    ),
}


class TestJsonEscapeControls(unittest.TestCase):
    def test_strict_parser_accepts_each_shape(self) -> None:
        header = '(require "std/json" all:)\n'
        for name, body in CASES.items():
            p = _run(header + body + "\n")
            self.assertEqual(p.returncode, 0, f"{name}: {p.stderr}")
            raw = p.stdout
            self.assertNotIn("\n", raw.replace("\\n", ""), f"{name} leaked a raw newline: {raw!r}")
            try:
                json.loads(raw)
            except json.JSONDecodeError as exc:
                self.fail(f"{name}: json.loads rejected {raw!r}: {exc}")


if __name__ == "__main__":
    unittest.main()
