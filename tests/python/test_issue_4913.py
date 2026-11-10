#!/usr/bin/env python3
"""Regression for #4913: aura new / run / test, a complete --help, soft --load.

Before: `aura new foo` was "cannot open program file 'new'", there was no
`aura run` / `aura test`, --help did not list --fmt/--typecheck/--query/…,
and `aura soft --load f` was "unknown option '--load'".
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

EVO = '(set-code "(define p 1)")\n(eval-current)\n(display (mutate:rebind "p" "2" "x"))\n(newline)\n'
_VERSION_LINE = re.compile(r"^aura (\S+) \(([0-9a-f]{9}|[0-9a-f]{40}|unknown)\)$")


def published_channel(build_version: str) -> str:
    """Same rule as project_channel_4913() in src/main.cpp.

    A release build version is exactly X.Y.Z (digits and two dots). That
    string is the image tag and the aura.toml pin. dev, nightly, latest,
    v0.1.0, and any other spelling are not a release channel and become
    nightly. :latest is never written.
    """
    if (
        build_version
        and build_version.count(".") == 2
        and not build_version.startswith(".")
        and not build_version.endswith(".")
        and all(c == "." or c.isdigit() for c in build_version)
    ):
        return build_version
    return "nightly"


def run(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH"):
        env.pop(k, None)
    return subprocess.run([str(AURA_BIN), *args], cwd=cwd, capture_output=True, text=True, env=env, timeout=120)


class FrontDoor(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def new(self, *args: str) -> Path:
        r = run(["new", *args], self.d)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return self.d / args[-1]

    def test_app(self) -> None:
        p = self.new("app", "demo")
        for f in (
            "aura.toml",
            "main.aura",
            "lib/greet.aura",
            "tests/greet_test.aura",
            "tests/greet_test.golden",
            ".github/workflows/ci.yml",
            "README.md",
        ):
            self.assertTrue((p / f).is_file(), f)
        self.assertIn("# demo", (p / "README.md").read_text())
        r = run(["run"], p)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("hello, world", r.stdout)
        self.assertIn("entry=main.aura", r.stderr)
        r = run(["run", "--quiet"], p / "lib")  # from a subdirectory
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr, "")
        r = run(["test"], p)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("1 passed, 0 failed", r.stdout)

    def test_channel_rule_rejects_unpublished_spellings(self) -> None:
        for bad in (
            "",
            "dev",
            "nightly",
            "latest",
            "v0.1.0",
            "0.1",
            "0.1.0.0",
            ".1.2",
            "1.2.",
            "0.1.0-rc1",
            "1.2.a",
        ):
            self.assertEqual(published_channel(bad), "nightly", bad)
        self.assertEqual(published_channel("0.1.0"), "0.1.0")
        self.assertEqual(published_channel("1.2.3"), "1.2.3")

    def test_ci_image_is_a_published_channel(self) -> None:
        # The tag is this binary's own release channel, not a hardcoded
        # nightly and not :latest. A 0.1.0 build writes :0.1.0; dev/nightly
        # writes :nightly. The registry is not queried: dist runs this test
        # before that tag is published.
        ver = run(["--version"], self.d)
        self.assertEqual(ver.returncode, 0, ver.stderr)
        line = ver.stdout.strip().splitlines()[-1]
        matched = _VERSION_LINE.fullmatch(line)
        self.assertIsNotNone(matched, line)
        channel = published_channel(matched.group(1))
        self.assertRegex(channel, r"^(nightly|\d+\.\d+\.\d+)$")
        other = "0.1.0" if channel == "nightly" else "nightly"
        for tpl in ("app", "evolve", "agent"):
            p = self.new(tpl, f"c-{tpl}")
            ci = (p / ".github/workflows/ci.yml").read_text()
            toml = (p / "aura.toml").read_text()
            self.assertNotIn("{{", ci + toml)
            self.assertNotIn(":latest", ci)
            self.assertNotIn(f"ghcr.io/cybrid-systems/aura:{other}\n", ci)
            self.assertIn(f"image: ghcr.io/cybrid-systems/aura:{channel}\n", ci)
            self.assertIn(f'[dependencies]\naura = "{channel}"\n', toml)
            r = run(["run", "--quiet"], p)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_evolve_worldline_under_5s(self) -> None:
        p = self.new("evolve", "demo")
        t0 = time.monotonic()
        r = run(["run"], p)
        self.assertLess(time.monotonic() - t0, 5.0)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("preset=dev", r.stderr)
        self.assertEqual(
            r.stdout,
            "1 ROLLBACK  (define (guess) 10)   live (define (guess) 0)\n"
            "2 KEEP      (define (guess) 7)    live (define (guess) 7)\n"
            "3 ROLLBACK  (define (guess) 2)    live (define (guess) 7)\n"
            "4 KEEP      (define (guess) 42)   live (define (guess) 42)\n"
            "(guess) => 42\n",
        )
        self.assertFalse((p / "report.aura").exists())
        self.assertFalse((p / "out" / "report.html").exists())
        self.assertFalse((p / "out" / "audit.tsv").exists())
        self.assertEqual(run(["test"], p).returncode, 0)

    def test_evolve_moo_reports_front_and_qd(self) -> None:
        p = self.new("evolve-moo", "demo")
        for f in (
            "aura.toml",
            "main.aura",
            "lib/moo.aura",
            "tests/moo_test.aura",
            "tests/moo_test.golden",
            ".github/workflows/ci.yml",
            "README.md",
        ):
            self.assertTrue((p / f).is_file(), f)
        t0 = time.monotonic()
        r = run(["run"], p)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertLess(time.monotonic() - t0, 60.0)
        self.assertIn("evaluated 240", r.stdout)
        self.assertIn("qd coverage", r.stdout)
        self.assertIn("wrote out/pareto.svg and out/qd.svg", r.stdout)
        self.assertIn("<svg", (p / "out" / "pareto.svg").read_text())
        self.assertIn("<rect", (p / "out" / "qd.svg").read_text())
        self.assertEqual(run(["test"], p).returncode, 0)
        # The run logged every candidate, so the report has objective and
        # behaviour space to draw: Pareto scatter, QD map, lineage.
        self.assertTrue((p / ".aura" / "evo" / "log.jsonl").is_file())
        rep = run(["evo", "report", "--html", "out/report"], p)
        self.assertEqual(rep.returncode, 0, rep.stderr)
        for f in ("pareto.svg", "hv-curve.svg", "qd-heatmap.svg", "lineage.svg"):
            self.assertTrue((p / "out" / "report" / f).is_file(), f)
        page = (p / "out" / "report" / "index.html").read_text()
        self.assertIn("Pareto front", page)
        self.assertIn("Hypervolume", page)
        self.assertIn("QD map", page)
        self.assertIn("Lineage", page)

    def test_agent(self) -> None:
        p = self.new("agent", "demo")
        r = run(["run"], p)
        self.assertEqual(
            r.stdout,
            "propose  (define (double x) (+ x x))\n"
            "check    (double 21) => 42  KEEP\n"
            "propose  (define (double x) (+ x 1))\n"
            "check    (double 21) => 22  REJECT\n"
            "live     (define (double x) (+ x x))\n",
            r.stderr,
        )
        self.assertEqual(run(["test"], p).returncode, 0)

    def test_default_template_and_errors(self) -> None:
        p = self.new("plain")
        self.assertTrue((p / "lib" / "greet.aura").is_file())
        r = run(["new", "app", "plain"], self.d)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not empty", r.stderr)
        r = run(["new", "nope", "x"], self.d)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unknown template", r.stderr)
        r = run(["run"], self.d)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("no aura.toml", r.stderr)
        self.assertIn("new app", r.stderr)

    def test_golden_diff_and_update(self) -> None:
        p = self.new("app", "demo")
        (p / "tests" / "greet_test.golden").write_text("hello, aura\n11\n")
        r = run(["test"], p)
        self.assertEqual(r.returncode, 1)
        self.assertIn("FAIL tests/greet_test.aura", r.stdout)
        self.assertIn("line 2", r.stdout)
        self.assertEqual(run(["test", "--update"], p).returncode, 0)
        self.assertEqual(run(["test"], p).returncode, 0)

    def test_help_lists_commands(self) -> None:
        r = run(["help", "--all"], self.d)  # #4935: --help is the short overview
        for w in (
            "new",
            "demo",
            "run",
            "test",
            "--fmt",
            "--check",
            "--typecheck",
            "--evo-explain",
            "--query",
            "--inspect",
            "--load",
            "--preset",
        ):
            self.assertIn(w, r.stdout, w)
        self.assertEqual(run(["--help-all"], self.d).stdout, r.stdout)

    def test_soft_load(self) -> None:
        (self.d / "evo.aura").write_text(EVO)
        for args in (["soft", "--load", "evo.aura"], ["--preset", "dev", "--load", "evo.aura"]):
            r = run(args, self.d)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("#t", r.stdout)

    def test_embedded_templates_current(self) -> None:
        out = self.d / "t.inc"
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts/tools/gen_cli_templates_4913.py"),
                str(ROOT / "templates/new"),
                str(out),
            ],
            check=True,
            capture_output=True,
        )
        self.assertEqual(
            out.read_text(),
            (ROOT / "src/cli_templates_4913.inc").read_text(),
            "run scripts/tools/gen_cli_templates_4913.py templates/new src/cli_templates_4913.inc",
        )


if __name__ == "__main__":
    unittest.main()
