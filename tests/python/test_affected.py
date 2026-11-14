#!/usr/bin/env python3
"""Fast-loop selection and run (#5232). Plan tests do not start Aura."""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AFFECTED_DOC, AURA_BIN  # noqa: E402
from affected import (  # noqa: E402
    CORE_PATHS,
    NEXT_HOME,
    NEXT_PR,
    command_exit,
    plan_paths,
)
from homes import HOMES, match_homes, path_hits  # noqa: E402
from run import main  # noqa: E402


def _last_json(text: str) -> dict:
    line = text.strip().splitlines()[-1]
    payload = json.loads(line)
    if not isinstance(payload, dict):
        raise AssertionError(line)
    return payload


class TestMap(unittest.TestCase):
    def test_roots_exist_and_are_in_the_genealogy(self) -> None:
        genealogy = (ROOT / "tests" / "HOMES.md").read_text(encoding="utf-8")
        self.assertIn("## Fast loop", genealogy)
        self.assertIn("python3 tests/run.py affected", genealogy)
        self.assertIn("./build.py test affected", genealogy)
        self.assertIn("./build.py test ci", genealogy)
        self.assertIn(NEXT_HOME, genealogy)
        self.assertIn(NEXT_PR, genealogy)
        seen: dict[str, str] = {}
        for home in HOMES:
            self.assertIn(home.path, genealogy, home.surface)
            for root in home.roots:
                self.assertIn(root, genealogy, root)
                self.assertNotIn(root, seen, root)
                seen[root] = home.surface
                target = ROOT / root.rstrip("/")
                if root.endswith("/"):
                    self.assertTrue(target.is_dir(), root)
                elif target.exists():
                    self.assertTrue(target.is_file(), root)
                else:
                    hits = [path.name for path in target.parent.iterdir() if path.name.startswith(target.name)]
                    self.assertTrue(hits, root)
            found = match_homes(home.path)
            self.assertEqual([item.surface for item in found], [home.surface], home.path)

    def test_string_diff_is_only_the_string_suite(self) -> None:
        plan = plan_paths(["lib/std/string.aura"])
        self.assertEqual(plan.aura, ("tests/suite/string_join.aura",))
        self.assertEqual(plan.python, ())
        self.assertEqual(plan.cpp, ())
        self.assertEqual(plan.unmapped, ())
        self.assertTrue(plan.runnable)
        self.assertTrue(plan.core)
        self.assertNotIn("tests/suite/orchestrator.aura", plan.aura)
        self.assertFalse(any(path.startswith("tests/orch/") for path in plan.aura))
        suite_n = len(list((ROOT / "tests" / "suite").glob("*.aura")))
        self.assertGreater(suite_n, 200)
        self.assertEqual(len(plan.aura), 1)

    def test_parser_diagnostic_is_only_the_parser_suite(self) -> None:
        for path in (
            "src/parser/parser_impl.cpp",
            "src/compiler/diag.ixx",
            "src/compiler/evaluator_primitives_diagnostic.cpp",
        ):
            plan = plan_paths([path])
            self.assertEqual(plan.aura, ("tests/suite/paren_semi_comment_5199.aura",), path)
            self.assertEqual(plan.python, (), path)
            self.assertEqual(len(plan.aura), 1, path)
            self.assertNotIn("tests/suite/string_join.aura", plan.aura)
            self.assertNotIn("tests/suite/orchestrator.aura", plan.aura)

    def test_python_part_selects_its_theme(self) -> None:
        plan = plan_paths(["tests/python/process/dloc_file_face.py"])
        self.assertEqual(plan.python, ("tests/python/process/test_diagnostic_location.py",))
        self.assertEqual(plan.aura, ())
        self.assertTrue(plan.core)

    def test_cpp_home_is_not_runnable(self) -> None:
        plan = plan_paths(["tests/core/test_arena_batch.cpp"])
        self.assertFalse(plan.runnable)
        self.assertEqual(plan.aura, ())
        self.assertEqual(len(plan.cpp), 1)
        self.assertEqual(plan.cpp[0].surface, "arena")
        self.assertEqual(command_exit(plan, ran=False, tests_ok=True), 1)

    def test_unmapped_exits_nonzero_with_a_next_step(self) -> None:
        plan = plan_paths(["README.md"])
        self.assertFalse(plan.runnable)
        self.assertEqual(plan.unmapped, ("README.md",))
        self.assertEqual(command_exit(plan, ran=False, tests_ok=True), 1)
        code = main(["affected", "--plan", "README.md"])
        self.assertEqual(code, 1)

    def test_mixed_unmapped_still_fails_after_a_green_run(self) -> None:
        plan = plan_paths(["lib/std/string.aura", "README.md"])
        self.assertTrue(plan.runnable)
        self.assertEqual(plan.unmapped, ("README.md",))
        self.assertEqual(command_exit(plan, ran=True, tests_ok=True), 1)

    def test_core_file_is_a_core_run(self) -> None:
        plan = plan_paths(["lib/std/test.aura"])
        self.assertTrue(plan.runnable)
        self.assertEqual(plan.aura, ())
        self.assertTrue(plan.core)
        for rel in CORE_PATHS:
            self.assertIn(rel, (ROOT / "tests" / "HOMES.md").read_text(encoding="utf-8"))

    def test_changed_suite_file_runs_itself(self) -> None:
        rel = "tests/suite/string_index_o1_4548.aura"
        self.assertTrue((ROOT / rel).is_file())
        plan = plan_paths([rel])
        self.assertEqual(plan.aura, (rel,))
        self.assertNotIn("tests/suite/string_join.aura", plan.aura)

    def test_fiber_stem_is_the_fiber_home(self) -> None:
        plan = plan_paths(["src/serve/fiber_rt_defaults.cpp"])
        self.assertEqual(plan.aura, ("tests/suite/issue_5064_fiber_cancel_text.aura",))
        self.assertTrue(path_hits("src/serve/fiber.cpp", "src/serve/fiber"))
        self.assertFalse(path_hits("src/serve/fibers_extra.cpp", "src/serve/fiber"))

    def test_orch_source_does_not_select_the_cpp_batch(self) -> None:
        plan = plan_paths(["src/orch/orch.ixx"])
        self.assertEqual(plan.aura, ("tests/suite/orchestrator.aura",))
        self.assertEqual(plan.cpp, ())
        self.assertFalse(any(path.startswith("tests/orch/") for path in plan.aura))

    def test_runner_source_does_not_build(self) -> None:
        text = (ROOT / "tests" / "python" / "affected.py").read_text(encoding="utf-8")
        self.assertNotIn("ninja", text)
        self.assertNotIn("cmake", text)
        self.assertNotIn("run_issue_tests", text)
        homes = (ROOT / "tests" / "python" / "homes.py").read_text(encoding="utf-8")
        self.assertNotIn("subprocess", homes)
        self.assertNotIn("ninja", homes)


class TestCli(unittest.TestCase):
    def test_plan_string_does_not_run(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run.py"), "affected", "--plan", "lib/std/string.aura"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("tests/suite/string_join.aura", proc.stdout)
        self.assertNotIn("orchestrator.aura", proc.stdout)
        self.assertNotIn("case PASS", proc.stdout)
        self.assertNotIn("ninja", proc.stdout)
        payload = _last_json(proc.stdout)
        self.assertFalse(payload["ran"])
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["aura"], ["tests/suite/string_join.aura"])

    def test_unmapped_cli_names_the_next_step(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run.py"), "affected", "README.md"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn('not found: no home for "README.md"', proc.stdout)
        self.assertIn(NEXT_HOME, proc.stdout)
        self.assertIn(NEXT_PR, proc.stdout)
        self.assertNotIn("case PASS", proc.stdout)
        self.assertNotIn("ninja", proc.stdout)
        payload = _last_json(proc.stdout)
        self.assertFalse(payload["ok"])
        self.assertEqual(payload["aura"], [])
        self.assertEqual(payload["cases"], [])

    def test_list_text_matches_build_py(self) -> None:
        listed = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run.py"), "list"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        built = subprocess.run(
            [sys.executable, str(ROOT / "build.py"), "list"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertEqual(built.returncode, 0, built.stderr)
        self.assertIn(AFFECTED_DOC, listed.stdout)
        self.assertIn(AFFECTED_DOC, built.stdout)
        self.assertIn("affected", built.stdout)
        doc = (ROOT / "build.py").read_text(encoding="utf-8").split('"""', 2)[1]
        self.assertIn(AFFECTED_DOC, doc)

    def test_build_py_plan_does_not_expand_into_ci(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(ROOT / "build.py"), "test", "affected", "--plan", "lib/std/string.aura"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("tests/suite/string_join.aura", proc.stdout)
        self.assertNotIn("case PASS", proc.stdout)
        self.assertNotIn("Issue Tests", proc.stdout)
        self.assertNotIn("ninja", proc.stdout)
        payload = _last_json(proc.stdout)
        self.assertEqual(payload["aura"], ["tests/suite/string_join.aura"])
        self.assertFalse(payload["ran"])


class TestRun(unittest.TestCase):
    def setUp(self) -> None:
        self.assertTrue(AURA_BIN.is_file(), AURA_BIN)

    def _run(self, path: str) -> dict:
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run.py"), "affected", path],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn("ninja", proc.stdout)
        self.assertNotIn("orchestrator.aura", proc.stdout)
        self.assertNotIn("Issue Tests", proc.stdout)
        payload = _last_json(proc.stdout)
        self.assertTrue(payload["ran"])
        self.assertTrue(payload["ok"], payload)
        self.assertEqual(payload["failed"], 0)
        self.assertGreater(payload["passed"], 0)
        names = {case["name"] for case in payload["cases"]}
        self.assertIn("std-test-loads", names)
        self.assertIn("basic_add", names)
        return payload

    def test_string_diff_runs_the_suite_and_core(self) -> None:
        payload = self._run("lib/std/string.aura")
        self.assertEqual(payload["aura"], ["tests/suite/string_join.aura"])
        self.assertEqual(payload["python"], [])
        self.assertEqual(payload["cpp_skipped"], [])
        issues = {case["issue"] for case in payload["cases"] if case["file"].endswith("string_join.aura")}
        self.assertIn(5185, issues)
        self.assertGreater(len(list((ROOT / "tests" / "suite").glob("*.aura"))), len(payload["aura"]))

    def test_parser_diff_runs_the_parser_suite_and_core(self) -> None:
        payload = self._run("src/parser/parser_impl.cpp")
        self.assertEqual(payload["aura"], ["tests/suite/paren_semi_comment_5199.aura"])
        self.assertNotIn("tests/suite/string_join.aura", payload["aura"])
        names = [case["name"] for case in payload["cases"] if case["kind"] == "aura"]
        self.assertEqual(names, ["closed paren-semi is a list"])
        issues = {case["issue"] for case in payload["cases"] if case["kind"] == "aura"}
        self.assertEqual(issues, {5199})


if __name__ == "__main__":
    unittest.main()
