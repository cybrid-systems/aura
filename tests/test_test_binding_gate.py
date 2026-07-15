#!/usr/bin/env python3
"""Unit tests for scripts/check_test_binding.py (#1453)."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "check_test_binding.py"


def _load():
    spec = importlib.util.spec_from_file_location("check_test_binding", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestBindingClassify(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = _load()

    def test_prod_primitives(self):
        self.assertTrue(self.m.is_prod("src/compiler/evaluator_primitives_memory.cpp"))
        self.assertTrue(self.m.is_prod("src/compiler/evaluator.ixx"))
        self.assertTrue(self.m.is_prod("src/compiler/service.ixx"))
        self.assertFalse(self.m.is_prod("src/core/ast.ixx"))

    def test_test_paths(self):
        self.assertTrue(self.m.is_test("tests/test_issue_1453.cpp"))
        self.assertTrue(self.m.is_test("tests/edsl_self_test.aura"))
        self.assertTrue(self.m.is_test("lib/std/edsl-test-harness.aura"))
        self.assertFalse(self.m.is_test("docs/design/foo.md"))

    def test_pairing_ok(self):
        code, msg = self.m.evaluate_binding(
            [
                "src/compiler/evaluator_primitives_memory.cpp",
                "tests/test_issue_300.cpp",
            ]
        )
        self.assertEqual(code, 0, msg)
        self.assertIn("paired", msg)

    def test_pairing_fail(self):
        code, msg = self.m.evaluate_binding(["src/compiler/evaluator_primitives_memory.cpp"])
        self.assertEqual(code, 1, msg)
        self.assertIn("FAIL", msg)

    def test_no_prod_ok(self):
        code, msg = self.m.evaluate_binding(["docs/contributing.md"])
        self.assertEqual(code, 0, msg)

    def test_added_public_names(self):
        diff = """
+++ b/src/compiler/evaluator_primitives_foo.cpp
@@
+    add("brand-new-prim-1453", [](const auto&) { return make_void(); });
+    // comment add("not-this")
-    add("removed-old", ...);
"""
        names = self.m.added_public_names(diff)
        self.assertIn("brand-new-prim-1453", names)
        self.assertNotIn("removed-old", names)


class TestRegistryGen(unittest.TestCase):
    def test_scan_nonempty(self):
        gen = ROOT / "scripts" / "gen_test_registry.py"
        spec = importlib.util.spec_from_file_location("gen_test_registry", gen)
        assert spec and spec.loader
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        data = mod.scan()
        self.assertGreater(data["count"], 100)
        self.assertIn("integration", data["by_category"])
        files = {t["file"] for t in data["tests"]}
        self.assertIn("tests/test_issue_1451.cpp", files)


def main() -> int:
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
