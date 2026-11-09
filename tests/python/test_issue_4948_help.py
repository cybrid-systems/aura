#!/usr/bin/env python3
"""Help snapshots for the v0.2 evolution modules (#4948).

#4935 pinned the CLI surface: every subcommand has --help, `aura help X` is
the same text, and help has no side effects. This file pins the other half of
the P7 DX promise for the v0.2 search stack:

  * every new module answers (xxx:help) with one stable page that names the
    module and its issue,
  * the same module is listed in lib/std/INDEX.aura (stdlib:list) and, when a
    .aura-type exists, the help symbol is part of the typed surface,
  * the CLI entry points that introduce the modules (`aura help new`,
    `aura new --help`, `aura help evo`, `aura evo log --help`, `aura --help`)
    keep their exact text.

The snapshots live in tests/fixtures/help_4948/. Regenerate them only after an
intentional help change:

    AURA_UPDATE_HELP_SNAPSHOTS=1 python3 tests/python/test_issue_4948_help.py

Run it the way tests/run.py runs issue tests (build/aura in ROOT/build, or
AURA_BUILD_DIR pointing at another build).
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

SNAPSHOTS = ROOT / "tests" / "fixtures" / "help_4948"
UPDATE = os.environ.get("AURA_UPDATE_HELP_SNAPSHOTS") == "1"

# (module, help symbol, the issue the page must name). swarm is here because
# #4948 made its multi-objective surface delegate to std/pareto.
MODULES: tuple[tuple[str, str, str], ...] = (
    ("evo-core", "evo:help", "#4939"),
    ("pareto", "pareto:help", "#4940"),
    ("moead", "moead:help", "#4940"),
    ("qd", "qd:help", "#4941"),
    ("proposer", "prop:help", "#4942"),
    ("surrogate", "surr:help", "#4943"),
    ("evo-meta", "meta:help", "#4944"),
    ("portfolio", "portfolio:help", "#4945"),
    ("algo-select", "select:help", "#4945"),
    ("hyper-heuristic", "hh:help", "#4945"),
    ("swarm", "swarm:help", "#4740"),
)

# (snapshot name, argv): the command-line half of the same promise.
CLI: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("cli-top", ("--help",)),
    ("cli-new-help", ("new", "--help")),
    ("cli-help-new", ("help", "new")),
    ("cli-new-evolve-moo-help", ("new", "evolve-moo", "--help")),
    ("cli-help-evo", ("help", "evo")),
    ("cli-evo-log-help", ("evo", "log", "--help")),
)


def run(args: tuple[str, ...] | list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_EVO_LOG"):
        env.pop(k, None)
    return subprocess.run(
        [str(AURA_BIN), *args], cwd=cwd, capture_output=True, text=True, env=env, timeout=120, stdin=subprocess.DEVNULL
    )


# The build version differs between a local build (dev) and a release or dist
# build (nightly / X.Y.Z), so pin the shape of the banner, not the version.
_VERSION_BANNER = re.compile(r"\Aaura \S+ — an AI-native Lisp")


def stable(text: str) -> str:
    return _VERSION_BANNER.sub("aura <version> — an AI-native Lisp", text, count=1)


def snapshot(name: str, actual: str) -> str:
    """Compare against tests/fixtures/help_4948/<name>.txt (or rewrite it)."""
    path = SNAPSHOTS / f"{name}.txt"
    if UPDATE:
        SNAPSHOTS.mkdir(parents=True, exist_ok=True)
        path.write_text(actual)
        return actual
    if not path.exists():
        raise AssertionError(
            f"missing help snapshot {path.relative_to(ROOT)}; regenerate with AURA_UPDATE_HELP_SNAPSHOTS=1"
        )
    return path.read_text()


class ModuleHelp(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_every_module_help_is_pinned(self) -> None:
        for module, symbol, issue in MODULES:
            with self.subTest(module=module):
                r = run(["-e", f'(require "std/{module}" all:)(display ({symbol}))'], self.d)
                self.assertEqual(r.returncode, 0, f"{module}: {r.stderr}")
                self.assertEqual(r.stderr, "", f"{module} help must not warn: {r.stderr}")
                out = r.stdout
                self.assertTrue(out.strip(), f"{module}: empty help")
                self.assertIn(f"std/{module}", out)
                self.assertIn(issue, out)
                self.assertEqual(out, snapshot(f"module-{module}", out), f"{module}: help text changed")

    def test_every_module_is_in_the_index(self) -> None:
        names = " ".join(f'"{m}"' for m, _, _ in MODULES)
        prog = (
            '(require "std/INDEX" all:)'
            '(for-each (lambda (m) (if (member m (stdlib:list)) #f (display (string-append m " ")))) '
            f"(list {names}))"
        )
        r = run(["-e", prog], self.d)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "", f"not in lib/std/INDEX.aura stdlib:list: {r.stdout}")

    def test_type_signature_has_help(self) -> None:
        for module, symbol, _ in MODULES:
            typed = ROOT / "lib" / "std" / f"{module}.aura-type"
            if not typed.exists():
                continue
            with self.subTest(module=module):
                lines = typed.read_text().splitlines()
                self.assertTrue(
                    any(line.startswith(f"{symbol}:") for line in lines),
                    f"{typed.relative_to(ROOT)} has no {symbol} signature",
                )


class CliHelp(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_cli_help_is_pinned(self) -> None:
        for name, args in CLI:
            with self.subTest(args=args):
                r = run(list(args), self.d)
                self.assertEqual(r.returncode, 0, f"{args}: {r.stderr}")
                self.assertEqual(r.stderr, "", f"{args} must not write stderr: {r.stderr}")
                out = stable(r.stdout)
                self.assertEqual(out, snapshot(name, out), f"{args}: help text changed")
        # Help is read-only: no template directory was scaffolded by --help.
        self.assertEqual(list(self.d.iterdir()), [])

    def test_help_names_the_new_surfaces(self) -> None:
        self.assertIn("evolve-moo", run(["help", "new"], self.d).stdout)
        evo_help = run(["help", "evo"], self.d).stdout
        for needle in ("--front", "--lineage", "Pareto", "HV curve", "QD heat map", "lineage tree"):
            self.assertIn(needle, evo_help)


if __name__ == "__main__":
    unittest.main()
