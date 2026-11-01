#!/usr/bin/env python3
"""Regression tests for issues #4847-#4858 (orch/COW/pipeline batch).

#4847 an orch agent that joined cleanly must not crash the process at exit
      (the pre-fix SIGSEGV in Fiber::~Fiber made every rc 139).
#4852 fiber:overlay-set! on an unbound name reports the binding name and the
      overlay-define hint instead of the literal " + name + ".
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


def _env() -> dict[str, str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    # Exercise the default policy: a stale inherited override would hide #4848.
    env.pop("AURA_PIPELINE_STRICT", None)
    return env


def _run(
    source: str, *args: str, timeout: float = 30.0, pipeline_strict: str | None = None
) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = _env()
    if pipeline_strict is not None:
        env["AURA_PIPELINE_STRICT"] = pipeline_strict
    try:
        return subprocess.run(
            [str(AURA_BIN), *args, path],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
        )
    finally:
        os.unlink(path)


class TestIssueBatch4847To4858(unittest.TestCase):
    def test_4847_agent_join_exits_clean(self) -> None:
        proc = _run(
            """
(orch:spawn-agent "w" (lambda () 1))
(display (hash-ref (orch:agent-join "w" :timeout-ms 3000) "status")) (newline)
""",
            "--load",
            timeout=60.0,
            pipeline_strict="0",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("ok", proc.stdout)
        self.assertNotIn("SIGSEGV", proc.stderr)

    def test_4852_overlay_set_unbound_names_binding(self) -> None:
        proc = _run(
            """
(fiber:overlay-begin)
(display (try (fiber:overlay-set! "never-defined-key" 5)
              (catch (e) (list 'err (format "~a" e))))) (newline)
(fiber:overlay-rollback)
""",
            "--load",
            pipeline_strict="0",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("unbound binding never-defined-key", proc.stdout)
        self.assertIn("fiber:overlay-define", proc.stdout)
        self.assertNotIn(" + name + ", proc.stdout)


if __name__ == "__main__":
    unittest.main()
