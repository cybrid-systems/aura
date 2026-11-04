#!/usr/bin/env python3
"""Regression for #4907: the default audit WAL dir is per-user.

Before: with no AURA_MUTATION_AUDIT_WAL / AURA_PERSIST_DIR the WAL went to
the shared $TMPDIR/aura-audit. When another user (or an earlier root
container) owned that path, every mutation of a freshly installed aura was
rejected with `security-schedule:wal-enable-failed` — `aura new evolve x &&
aura run` failed on first contact. Now the default is $TMPDIR/aura-audit-<euid>.
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

EVO = '(set-code "(define p 1)")\n(eval-current)\n(display (mutate:rebind "p" "2" "x"))\n(newline)\n'


class PerUserWalDir(unittest.TestCase):
    def test_foreign_shared_dir_does_not_block_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            tmp = Path(t)
            # Stand-in for "/tmp/aura-audit belongs to someone else": a path
            # this process cannot use as its WAL directory.
            (tmp / "aura-audit").write_text("not yours\n")
            (tmp / "evo.aura").write_text(EVO)
            env = dict(os.environ)
            for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_MUTATION_AUDIT_WAL", "AURA_PERSIST_DIR", "TMP"):
                env.pop(k, None)
            env["TMPDIR"] = str(tmp)
            r = subprocess.run(
                [str(AURA_BIN), "--preset", "dev", "--quiet", "evo.aura"],
                cwd=tmp,
                capture_output=True,
                text=True,
                env=env,
                timeout=120,
            )
            out = r.stdout + r.stderr
            self.assertNotIn("wal-enable-failed", out)
            self.assertEqual(r.returncode, 0, out)
            self.assertTrue((tmp / f"aura-audit-{os.geteuid()}").is_dir(), out)


if __name__ == "__main__":
    unittest.main()
