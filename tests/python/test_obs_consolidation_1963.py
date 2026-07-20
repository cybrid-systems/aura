#!/usr/bin/env python3
"""Issue #1963 — obs_eval_NN + obs_jit_NN consolidation invariants."""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMPILER = ROOT / "src" / "compiler"


class ObsConsolidation1963(unittest.TestCase):
    def test_consolidated_tus_exist(self) -> None:
        self.assertTrue((COMPILER / "evaluator_primitives_obs_eval.cpp").is_file())
        self.assertTrue((COMPILER / "evaluator_primitives_obs_jit.cpp").is_file())

    def test_no_numbered_split_files(self) -> None:
        leftovers = sorted(p.name for p in COMPILER.glob("evaluator_primitives_obs_eval_*.cpp")) + sorted(
            p.name for p in COMPILER.glob("evaluator_primitives_obs_jit_*.cpp")
        )
        self.assertEqual(
            leftovers,
            [],
            f"orphan numbered splits remain (should be deleted post-#1963): {leftovers}",
        )

    def test_consolidated_headers_cite_1963(self) -> None:
        eval_txt = (COMPILER / "evaluator_primitives_obs_eval.cpp").read_text(encoding="utf-8", errors="replace")[:500]
        jit_txt = (COMPILER / "evaluator_primitives_obs_jit.cpp").read_text(encoding="utf-8", errors="replace")[:500]
        self.assertIn("Consolidated", eval_txt)
        self.assertIn("Consolidated", jit_txt)
        # Bodies still present in the mega-TUs
        self.assertIn(
            "register_eval_p",
            eval_txt
            + (COMPILER / "evaluator_primitives_obs_eval.cpp").read_text(encoding="utf-8", errors="replace")[
                10000:11000
            ],
        )
        full_eval = (COMPILER / "evaluator_primitives_obs_eval.cpp").read_text(encoding="utf-8", errors="replace")
        full_jit = (COMPILER / "evaluator_primitives_obs_jit.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("register_eval_p80", full_eval)
        self.assertIn("register_eval_p104", full_eval)
        self.assertIn("register_jit_p80", full_jit)
        self.assertIn("register_jit_p113", full_jit)

    def test_cmake_lists_only_consolidated(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("evaluator_primitives_obs_eval.cpp", cmake)
        self.assertIn("evaluator_primitives_obs_jit.cpp", cmake)
        self.assertNotIn("evaluator_primitives_obs_eval_10.cpp", cmake)
        self.assertNotIn("evaluator_primitives_obs_jit_10.cpp", cmake)


if __name__ == "__main__":
    unittest.main()
