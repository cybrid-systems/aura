"""#5063: --worker-threads before --load must still load the file.

The flag used to stay in argv (the parser returned before stripping it) and
the oneshot reader then reported `unknown option '--load'`.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


class WorkerThreadsBeforeLoad(unittest.TestCase):
    def test_flag_before_load_runs_the_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "w.aura"
            src.write_text("(display (+ 3 4))\n")
            for args in (
                ["--worker-threads", "1", "--load", str(src)],
                ["--worker-threads=4", "--load", str(src)],
                ["--load", str(src), "--worker-threads", "4"],
            ):
                r = subprocess.run(
                    [str(AURA_BIN), *args],
                    capture_output=True,
                    text=True,
                    timeout=30,
                    env={
                        **dict(**{k: v for k, v in __import__("os").environ.items()}),
                        "AURA_SANDBOX": "off",
                        "AURA_PIPELINE_STRICT": "0",
                    },
                )
                self.assertEqual(r.returncode, 0, (args, r.stderr))
                self.assertNotIn("unknown option", r.stderr)
                self.assertEqual(r.stdout.strip(), "7")


if __name__ == "__main__":
    unittest.main()
