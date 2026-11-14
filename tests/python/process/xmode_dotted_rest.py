#!/usr/bin/env python3
"""Regression for #4900: force-soa dotted-rest macro binding must not be empty.

Under AURA_PIPELINE_STRICT=force-soa a hygienic macro with a `. rest`
parameter expanded through the runtime walker (the macro was registered by
an earlier top-level form, so the static pass never saw it). The rest
substitution was a (list ...) call, and the walker re-evaluated the
resulting data list as code, which dropped it to void: `(m 1 2 3)` printed
`()` instead of `(2 3)`. The static face already printed `(2 3)`.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def _run(source: str, direct: bool, strict: str):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    if strict:
        env["AURA_PIPELINE_STRICT"] = strict
    else:
        env.pop("AURA_PIPELINE_STRICT", None)
    argv = [str(AURA_BIN)] + ([] if direct else ["--load"]) + [path]
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


REST = (
    "(define-hygienic-macro (m a . rest) rest)\n"
    "(display (m 1 2 3)) (newline)\n"
    "(define (g) (m 1 2 3))\n"
    "(display (g)) (newline)\n"
)


class TestForceSoaRestBinding4900(unittest.TestCase):
    def test_rest_binding_matches_static_face(self) -> None:
        for strict in ("force-soa", "0", ""):
            for direct in (True, False):
                p = _run(REST, direct=direct, strict=strict)
                tag = f"strict={strict or 'default'} direct={direct}"
                self.assertEqual(p.returncode, 0, f"{tag}: {p.stderr}")
                self.assertEqual(p.stdout.strip().splitlines()[-2:], ["(2 3)", "(2 3)"], tag)

    def test_single_toplevel_call(self) -> None:
        src = "(define-hygienic-macro (m a . rest) rest)\n(display (m 1 2 3))\n"
        for strict in ("force-soa", "0", ""):
            p = _run(src, direct=True, strict=strict)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stdout.strip(), "(2 3)", strict or "default")


if __name__ == "__main__":
    unittest.main()
