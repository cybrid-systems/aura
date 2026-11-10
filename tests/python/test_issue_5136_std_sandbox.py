#!/usr/bin/env python3
"""Issue #5136: the sandbox preset applies to lib/std modules too.

A stdlib module that needs an effect must be refused by the default
(production) face with a catchable error, not by an abort, and the same
require must succeed once the sandbox is explicitly off. Before this test
the property was only implied by the cross-mode corpus, which pins
stdout/rc parity but never asserts the denial itself.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

PROGRAM = """(display (try (begin (require "std/ffi" all:) (quote ok))
                        (catch (e) (quote denied))))
(newline)
"""


def run(face_env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("AURA_")}
    env.update(face_env)
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as fh:
        fh.write(PROGRAM)
        path = fh.name
    return subprocess.run(
        [str(AURA_BIN), "--load", path],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
        cwd=str(ROOT),
    )


class StdSandboxPreset(unittest.TestCase):
    def test_default_face_denies_and_stays_catchable(self) -> None:
        r = run({"AURA_PATH": str(ROOT / "lib")})
        self.assertEqual(r.returncode, 0, r.stderr[-800:])
        self.assertIn("denied", r.stdout)
        self.assertNotIn("CRASH", r.stderr)
        self.assertNotIn("SIGABRT", r.stderr)

    def test_sandbox_off_allows_the_same_require(self) -> None:
        r = run({"AURA_PATH": str(ROOT / "lib"), "AURA_SANDBOX": "off"})
        self.assertEqual(r.returncode, 0, r.stderr[-800:])
        self.assertIn("ok", r.stdout)


if __name__ == "__main__":
    unittest.main()
