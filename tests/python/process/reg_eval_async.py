#!/usr/bin/env python3
"""Issue #4880: --serve-async eval:async results.

Issue #4887 removed the legacy enqueue primitive; eval:async is the
supported path.


The old pool wiring treated the first eventfd wake (a stdin broadcast) as
completion, so both primitives returned an empty value, and the pool then
wrote the submitting fiber's eventfd after it was closed. This test drives
the issue's exact repro through --serve-async and checks the values plus
the absence of an EBADF write.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

REQUESTS = [
    {"session": "default", "cmd": "exec", "code": '(eval:async "(+ 1 2)")'},
    {
        "session": "default",
        "cmd": "exec",
        "code": ('(eval:async "(begin (define (s n a) (if (= n 0) a (s (- n 1) (+ a 1)))) (s 3000000 0))")'),
    },
]


class TestIssue4880(unittest.TestCase):
    def test_serve_async_results(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")

        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env.pop("AURA_PIPELINE_STRICT", None)
        payload = "".join(json.dumps(r) + "\n" for r in REQUESTS)
        try:
            proc = subprocess.run(
                [str(AURA_BIN), "--serve-async"],
                input=payload,
                text=True,
                capture_output=True,
                timeout=30,
                env=env,
            )
        except subprocess.TimeoutExpired:
            self.fail("--serve-async did not finish the #4880 repro (hang)")

        rows = []
        for line in proc.stdout.splitlines():
            line = line.strip()
            if line.startswith("{"):
                rows.append(json.loads(line))
        self.assertEqual(
            len(rows),
            len(REQUESTS),
            f"rows={rows!r} stdout={proc.stdout[-600:]!r} stderr={proc.stderr[-600:]!r}",
        )
        self.assertEqual(rows[0].get("status"), "ok")
        self.assertEqual(rows[0].get("value"), '"3"')
        self.assertEqual(rows[1].get("status"), "ok")
        self.assertEqual(rows[1].get("value"), '"3000000"')
        self.assertNotIn("Bad file descriptor", proc.stderr)


if __name__ == "__main__":
    unittest.main()
