#!/usr/bin/env python3
"""Regression for #5171: tool positionals that name a file are that file.

--typecheck, --auto-fix, --ir, and --query used to parse the path as
source text (`unbound variable: main`). --fmt and --unparse already
read a path. An existing file is now the program; any other positional
stays source text; a missing path is a read error. AURA_SANDBOX stays
unset.
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

BUG = '(display (string-ref "abc" "x"))\n'
OK = "(+ 1 2)\n"
DIAG = "expected Int, got String"
UNBOUND_PATH = "unbound variable: main"
CANNOT = "cannot read"


def run_aura(
    args: list[str],
    text: str = "",
    cwd: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(key, None)
    env["AURA_RUNTIME_DIR"] = str(ROOT)

    def invoke(work: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(AURA_BIN), *args],
            input=text,
            cwd=work,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )

    if cwd is not None:
        return invoke(cwd)
    with tempfile.TemporaryDirectory() as tmp:
        return invoke(Path(tmp))


class TestIssue5171ToolFile(unittest.TestCase):
    def test_typecheck_reads_the_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "main.aura"
            path.write_text(BUG, encoding="utf-8")
            file = run_aura(["--typecheck", "main.aura"], cwd=root)
            stdin = run_aura(["--typecheck"], BUG, cwd=root)
            blob = file.stdout + file.stderr
            self.assertEqual(file.returncode, 1, blob)
            self.assertEqual(file.stdout, stdin.stdout)
            self.assertIn(DIAG, file.stdout)
            self.assertNotIn(UNBOUND_PATH, blob)
            self.assertNotIn("unbound variable", blob)

            absolute = run_aura(["--typecheck", str(path)], cwd=root)
            self.assertEqual(absolute.returncode, 1, absolute.stdout)
            self.assertEqual(absolute.stdout, stdin.stdout)

            quiet = run_aura(["--quiet", "--typecheck", "main.aura"], cwd=root)
            self.assertEqual(quiet.returncode, 1, quiet.stdout + quiet.stderr)
            self.assertEqual(quiet.stdout, stdin.stdout)

            clean = root / "ok.aura"
            clean.write_text(OK, encoding="utf-8")
            ok = run_aura(["--typecheck", "ok.aura"], cwd=root)
            self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
            self.assertIn("type: Int", ok.stdout)
            self.assertIn("no errors", ok.stdout)

    def test_source_text_and_missing_paths(self) -> None:
        inline = run_aura(["--typecheck", "(+ 1 2)"])
        blob = inline.stdout + inline.stderr
        self.assertEqual(inline.returncode, 0, blob)
        self.assertIn("type: Int", inline.stdout)
        self.assertIn("no errors", inline.stdout)
        self.assertNotIn(CANNOT, blob)

        undef = run_aura(["--typecheck", "(undef 5)"])
        self.assertEqual(undef.returncode, 1, undef.stdout)
        self.assertIn("unbound variable: undef", undef.stdout)
        self.assertNotIn(CANNOT, undef.stdout + undef.stderr)

        slash = run_aura(["--typecheck", '(string-append "a/b" "c")'])
        self.assertEqual(slash.returncode, 0, slash.stdout + slash.stderr)
        self.assertIn("type: String", slash.stdout)
        self.assertNotIn(CANNOT, slash.stderr)

        missing = run_aura(["--typecheck", "missing.aura"])
        blob = missing.stdout + missing.stderr
        self.assertEqual(missing.returncode, 1, blob)
        self.assertIn(CANNOT, missing.stderr)
        self.assertIn("missing.aura", missing.stderr)
        self.assertNotIn("unbound variable", blob)

    def test_query_reads_the_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "main.aura").write_text(BUG, encoding="utf-8")
            file = run_aura(["--query", ":defines", "main.aura"], cwd=root)
            stdin = run_aura(["--query", ":defines"], BUG, cwd=root)
            blob = file.stdout + file.stderr
            self.assertEqual(file.returncode, 0, blob)
            self.assertEqual(file.stdout, stdin.stdout)
            self.assertIn("query: 6 matches", file.stdout)
            self.assertNotIn("query: 3 matches", file.stdout)

            missing = run_aura(["--query", ":defines", "missing.aura"], cwd=root)
            self.assertEqual(missing.returncode, 1, missing.stderr)
            self.assertIn(CANNOT, missing.stderr)

    def test_ir_auto_fix_fmt_and_unparse_agree(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "ok.aura").write_text(OK, encoding="utf-8")
            ir = run_aura(["--ir", "ok.aura"], cwd=root)
            self.assertEqual(ir.returncode, 0, ir.stdout + ir.stderr)
            self.assertEqual(ir.stdout.strip(), "3")
            missing_ir = run_aura(["--ir", "missing.aura"], cwd=root)
            self.assertEqual(missing_ir.returncode, 1)
            self.assertIn(CANNOT, missing_ir.stderr)

            fixed = run_aura(["--auto-fix", "ok.aura"], cwd=root)
            inline = run_aura(["--auto-fix", "(+ 1 2)"], cwd=root)
            self.assertEqual(fixed.returncode, 0, fixed.stdout + fixed.stderr)
            self.assertEqual(fixed.stdout, inline.stdout)
            self.assertIn("result: 3", fixed.stdout)
            missing_fix = run_aura(["--auto-fix", "no-such.aura"], cwd=root)
            self.assertIn(CANNOT, missing_fix.stderr)

            fmt = run_aura(["--fmt", "--check", "ok.aura"], cwd=root)
            self.assertEqual(fmt.returncode, 0, fmt.stdout + fmt.stderr)
            self.assertIn("formatted OK", fmt.stderr)
            typed = run_aura(["--typecheck", "ok.aura"], cwd=root)
            self.assertEqual(typed.returncode, 0, typed.stdout)
            self.assertIn("no errors", typed.stdout)

            as_code = run_aura(["--fmt", "(+ 1 2)"], cwd=root)
            self.assertEqual(as_code.returncode, 0, as_code.stdout + as_code.stderr)
            self.assertIn("(+ 1 2)", as_code.stdout)
            self.assertNotIn(CANNOT, as_code.stderr)
            gone = run_aura(["--fmt", "gone.aura"], cwd=root)
            self.assertEqual(gone.returncode, 1)
            self.assertIn("cannot read gone.aura", gone.stderr)

            unparsed = run_aura(["--unparse", "ok.aura"], cwd=root)
            self.assertEqual(unparsed.returncode, 0, unparsed.stderr)
            self.assertIn("written to ok.aura.fmt", unparsed.stderr)
            written = (root / "ok.aura.fmt").read_text(encoding="utf-8")
            self.assertIn("(+ 1 2)", written)

    def test_help_states_the_rule(self) -> None:
        detailed = run_aura(["help", "--all"])
        blob = detailed.stdout + detailed.stderr
        self.assertEqual(detailed.returncode, 0, blob)
        self.assertIn("existing file", blob)
        self.assertIn("source text", blob)
        self.assertIn("missing path", blob)
        self.assertIn("--typecheck", blob)
        self.assertIn("--fmt", blob)
        self.assertIn("--query", blob)

        short = run_aura(["--help"])
        blob = short.stdout + short.stderr
        self.assertEqual(short.returncode, 0, blob)
        self.assertIn("exists as a file", blob)
        self.assertIn("source text", blob)


if __name__ == "__main__":
    unittest.main()
