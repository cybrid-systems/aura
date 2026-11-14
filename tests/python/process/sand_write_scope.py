#!/usr/bin/env python3
"""Regression for #4912: path-scoped Write (`--grant write:out/`, the ci preset).

Before: `--preset ci` was the same as dev (Write on every host path) and
`--grant write:out/` was an unknown capability. Now a scoped Write lets
write-file / make-directory / file-copy / file-delete / serialize-workspace
write only under the listed directories; a path outside them (including
`..` and a symlink that leaves the root) is an effect-denied refusal with a
hint, and the caps banner names the scope. dev and a bare `--grant write`
stay unscoped.
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

PROG = """(require "std/io")
(define (ok? v) (if (error? v) "denied" v))
(display (list "mk" (ok? (make-directory "out")))) (newline)
(display (list "in" (ok? (write-file "out/a.txt" "in")))) (newline)
(display (list "sub" (ok? (make-directory "out/sub")))) (newline)
(display (list "cp-in" (ok? (file-copy "out/a.txt" "out/sub/b.txt")))) (newline)
(display (list "rm-in" (ok? (file-delete "out/sub/b.txt")))) (newline)
(display (list "outside" (ok? (write-file "x.txt" "no")))) (newline)
(display (list "dotdot" (ok? (write-file "out/../y.txt" "no")))) (newline)
(display (list "link" (ok? (write-file "out/link/z.txt" "no")))) (newline)
(display (list "cp-out" (ok? (file-copy "out/a.txt" "c.txt")))) (newline)
(display (list "rm-out" (ok? (file-delete "keep.txt")))) (newline)
(display (list "ser" (serialize-workspace "ws.bin"))) (newline)
(display (list "ser-in" (serialize-workspace "out/ws.bin"))) (newline)
"""

SCOPED = [
    "(mk 1)",
    "(in 1)",
    "(cp-in 1)",
    "(rm-in 1)",
    "(ser #f)",
    "(ser-in #t)",
]


class WriteScope(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.cwd = Path(self.dir.name)
        (self.cwd / "w.aura").write_text(PROG)
        (self.cwd / "keep.txt").write_text("keep")
        # out/link -> project root: a write through it leaves out/.
        (self.cwd / "out").mkdir()
        os.symlink(self.cwd, self.cwd / "out" / "link")

    def tearDown(self) -> None:
        self.dir.cleanup()

    def run_aura(self, *args: str, entry: str = "w.aura", **extra: str):
        env = dict(os.environ)
        for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
            env.pop(k, None)
        env.update(extra)
        return subprocess.run(
            [str(AURA_BIN), *args, *([entry] if entry else [])],
            cwd=self.cwd,
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )

    def assert_scoped(self, r: subprocess.CompletedProcess[str]) -> None:
        out = r.stdout + r.stderr
        for want in SCOPED:
            self.assertIn(want, r.stdout, out)
        # Out-of-scope writes are effect-denied errors (keep-going continues).
        for frag in (
            "for x.txt",
            "for out/../y.txt",
            "for out/link/z.txt",
            "for c.txt (writable: out/) op=file-copy",
            "for keep.txt (writable: out/) op=file-delete",
        ):
            self.assertIn("effect-denied: write-path not granted " + frag, r.stderr, out)
        self.assertEqual((self.cwd / "out" / "a.txt").read_text(), "in")
        self.assertTrue((self.cwd / "out" / "ws.bin").exists())
        for name in ("x.txt", "y.txt", "z.txt", "c.txt", "ws.bin"):
            self.assertFalse((self.cwd / name).exists(), name)
        self.assertEqual((self.cwd / "keep.txt").read_text(), "keep")

    def test_grant_write_dir_all_pipelines(self) -> None:
        for strict in ("", "force-soa"):
            with self.subTest(strict=strict):
                for name in ("x.txt", "y.txt", "z.txt", "c.txt", "ws.bin"):
                    (self.cwd / name).unlink(missing_ok=True)
                extra = {"AURA_PIPELINE_STRICT": strict} if strict else {}
                r = self.run_aura("--grant", "write:out/", **extra)
                self.assert_scoped(r)
                self.assertIn("caps=mutate,read,write:out/ ", r.stderr)

    def test_ci_preset_scopes_to_out(self) -> None:
        r = self.run_aura("--preset", "ci")
        self.assert_scoped(r)
        self.assertIn("preset=ci", r.stderr)
        self.assertIn("write:out/", r.stderr)

    def test_denial_is_classified_with_hint(self) -> None:
        (self.cwd / "d.aura").write_text('(write-file "x.txt" "no")\n')
        r = self.run_aura("--preset", "ci", entry="d.aura")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("effect-denied: write-path not granted for x.txt (writable: out/)", r.stderr)
        self.assertIn("--grant write:<dir>/", r.stderr)
        self.assertFalse((self.cwd / "x.txt").exists())

    def test_project_run_ci_uses_toml_out(self) -> None:
        (self.cwd / "aura.toml").write_text('entry = "w.aura"\nout = "out"\npreset = "ci"\n')
        r = self.run_aura("run", entry="")
        self.assert_scoped(r)

    def test_dev_and_bare_write_are_unscoped(self) -> None:
        for args in (("--preset", "dev"), ("--grant", "write"), ("--preset", "ci", "--grant", "write")):
            with self.subTest(args=args):
                for name in ("x.txt", "y.txt"):
                    (self.cwd / name).unlink(missing_ok=True)
                r = self.run_aura(*args)
                self.assertIn("(outside 1)", r.stdout, r.stdout + r.stderr)
                self.assertIn("(dotdot 1)", r.stdout)
                self.assertEqual((self.cwd / "x.txt").read_text(), "no")
                self.assertNotIn("write:out/", r.stderr)

    def test_default_still_denies_write(self) -> None:
        r = self.run_aura()
        self.assertIn("write not granted", r.stdout + r.stderr)
        self.assertFalse((self.cwd / "out" / "a.txt").exists())


if __name__ == "__main__":
    unittest.main()
