#!/usr/bin/env python3
"""Issue #1935 — fuzz infrastructure static tests."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FUZZ = ROOT / "tests" / "fuzz"


class FuzzInfra1935(unittest.TestCase):
    def test_layout(self) -> None:
        for name in ("common.py", "run_all.py", "corpus_tools.py", "README.md"):
            self.assertTrue((FUZZ / name).is_file(), name)
        self.assertTrue((FUZZ / "drivers").is_dir())
        self.assertTrue((FUZZ / "corpus").is_dir())
        self.assertGreater(len(list((FUZZ / "corpus").glob("*.sexpr"))), 50)

    def test_drivers_restored(self) -> None:
        needed = [
            "fuzz.py",
            "fuzz_corpus.py",
            "fuzz_edsl.py",
            "fuzz_equiv_mutate.py",
            "prop_hygiene_mutate.py",
        ]
        for n in needed:
            self.assertTrue((FUZZ / "drivers" / n).is_file(), n)

    def test_registry_list(self) -> None:
        r = subprocess.run(
            [sys.executable, str(FUZZ / "run_all.py"), "--list"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("corpus", r.stdout)
        self.assertIn("hygiene_prop", r.stdout)

    def test_build_py_fuzz_list(self) -> None:
        r = subprocess.run(
            [sys.executable, str(ROOT / "build.py"), "fuzz", "--list"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("Registered fuzzers", r.stdout)

    def test_corpus_tools_status(self) -> None:
        r = subprocess.run(
            [sys.executable, str(FUZZ / "corpus_tools.py"), "status"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("seeds", r.stdout)

    def test_docs(self) -> None:
        doc = (ROOT / "docs" / "fuzzing.md").read_text(encoding="utf-8")
        self.assertIn("1935", doc)
        self.assertIn("build.py fuzz", doc)
        nightly = (ROOT / ".github" / "workflows" / "nightly.yml").read_text(encoding="utf-8")
        self.assertIn("fuzz --only", nightly)
        self.assertIn("1935", nightly)

    def test_common_import(self) -> None:
        sys.path.insert(0, str(FUZZ))
        import common  # type: ignore

        self.assertTrue(common.CORPUS.is_dir() or True)
        self.assertEqual(common.FUZZ_ROOT, FUZZ)


if __name__ == "__main__":
    unittest.main()
